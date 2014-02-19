#include "Common.h"

using namespace std;
using namespace gl;
using namespace OVR;

const Resource SCENE_IMAGES[2] = {
  Resource::IMAGES_TUSCANY_UNDISTORTED_LEFT_PNG,
  Resource::IMAGES_TUSCANY_UNDISTORTED_RIGHT_PNG
};

#define DISTORTION_TIMING 1
class ShaderLookupDistort : public RiftGlfwApp {
protected:
  typedef Texture<GL_TEXTURE_2D, GL_RG16F> LookupTexture;
  typedef LookupTexture::Ptr LookupTexturePtr;

  glm::uvec2 lookupTextureSize;
  Texture2dPtr sceneTextures[2];
  LookupTexturePtr lookupTextures[2];
  GeometryPtr quadGeometry;
  float K[4];
  float lensOffset;

public:
  ShaderLookupDistort() : lookupTextureSize(512, 512) {
    OVR::HMDInfo hmdInfo;
    Rift::getHmdInfo(ovrManager, hmdInfo);
    OVR::Util::Render::StereoConfig stereoConfig;
    stereoConfig.SetHMDInfo(hmdInfo);
    const OVR::Util::Render::DistortionConfig & distortion = 
      stereoConfig.GetDistortionConfig();

    // The Rift examples use a post-distortion scale to resize the
    // image upward after distorting it because their K values have
    // been chosen such that they always result in a scale > 1.0, and
    // thus shrink the image.  However, we can correct for that by
    // finding the distortion scale the same way the OVR examples do,
    // and then pre-multiplying the constants by it.
    double postDistortionScale = 1.0 / stereoConfig.GetDistortionScale();
    for (int i = 0; i < 4; ++i) {
      K[i] = (float)(distortion.K[i] * postDistortionScale);
    }
    lensOffset = 1.0f - (2.0f *
      hmdInfo.LensSeparationDistance /
      hmdInfo.HScreenSize);
  }

  glm::vec2 findSceneTextureCoords(int eyeIndex, glm::vec2 texCoord) {
    static bool init = false;
    if (!init) {
      init = true;
    }

    // In order to calculate the distortion, we need to know
    // the distance between the lens axis point, and the point
    // we are rendering.  So we have to move the coordinate
    // from texture space (which has a range of [0, 1]) into
    // screen space
    texCoord *= 2.0;
    texCoord -= 1.0;

    // Moving it into screen space isn't enough though.  We
    // actually need it in rift space.  Rift space differs
    // from screen space in two ways.  First, it has
    // homogeneous axis scales, so we need to correct for any
    // difference between the X and Y viewport sizes by scaling
    // the y axis by the aspect ratio
    texCoord.y /= eyeAspect;

    // The second way rift space differs is that the origin
    // is at the intersection of the display pane with the
    // lens axis.  So we have to offset the X coordinate to
    // account for that.
    texCoord.x += (eyeIndex == 0) ? -lensOffset : lensOffset;

    // Although I said we need the distance between the
    // origin and the texture, it turns out that getting
    // the scaling factor only requires the distance squared.
    // We could get the distance and then square it, but that
    // would be a waste since getting the distance in the
    // first place requires doing a square root.
    float rSq = glm::length2(texCoord);
    float distortionScale = K[0] + rSq * (K[1] + rSq * (K[2] + rSq * K[3]));
    texCoord *= distortionScale;

    // Now that we've applied the distortion scale, we
    // need to reverse all the work we did to move from
    // texture space into Rift space.  So we apply the
    // inverse operations in reverse order.
    texCoord.x -= (eyeIndex == 0) ? -lensOffset : lensOffset;;
    texCoord.y *= eyeAspect;
    texCoord += 1.0;
    texCoord /= 2.0;

    // Our texture coordinate is now distorted.  Or rather,
    // for the input texture coordinate on the distorted screen
    // we now have the undistorted source coordinates to pull
    // the color from
    return texCoord;
  }

  void createLookupTexture(LookupTexturePtr & outTexture, int eyeIndex) {
    size_t lookupDataSize = lookupTextureSize.x * lookupTextureSize.y * 2;
    float * lookupData = new float[lookupDataSize];
    // The texture coordinates are actually from the center of the pixel, so thats what we need to use for the calculation.
    glm::vec2 texCenterOffset = glm::vec2(0.5f) / glm::vec2(lookupTextureSize);
    size_t rowSize = lookupTextureSize.x * 2;
    for (size_t y = 0; y < lookupTextureSize.y; ++y) {
      for (size_t x = 0; x < lookupTextureSize.x; ++x) {
        size_t offset = (y * rowSize) + (x * 2);
        glm::vec2 texCoord = (glm::vec2(x, y) / glm::vec2(lookupTextureSize)) + texCenterOffset;
        glm::vec2 sceneTexCoord = findSceneTextureCoords(eyeIndex, texCoord);
        lookupData[offset] = sceneTexCoord.x;
        lookupData[offset + 1] = sceneTexCoord.y;
      }
    }

    outTexture = LookupTexturePtr(new LookupTexture());
    outTexture->bind();
    outTexture->image2d(lookupTextureSize, lookupData, 0, GL_RG, GL_FLOAT);
    outTexture->parameter(GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    outTexture->parameter(GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    outTexture->parameter(GL_TEXTURE_WRAP_S, GL_MIRRORED_REPEAT);
    outTexture->parameter(GL_TEXTURE_WRAP_T, GL_MIRRORED_REPEAT);
    delete[] lookupData;
  }

  void initGl() {
    RiftGlfwApp::initGl();
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);

    quadGeometry = GlUtils::getQuadGeometry();

    // Generate the lookup textures and load the scene textures
    for (int eyeIndex = 0; eyeIndex < 2; ++eyeIndex) {
      GlUtils::getImageAsTexture(sceneTextures[eyeIndex], SCENE_IMAGES[eyeIndex]);
      createLookupTexture(lookupTextures[eyeIndex], eyeIndex);
    }
  }

  void draw() {
    glClear(GL_COLOR_BUFFER_BIT);
    for (int eyeIndex = 0; eyeIndex < 2; ++eyeIndex) {
      renderEye(eyeIndex);
    }
    GL_CHECK_ERROR;
  }

  void renderEye(int eyeIndex) {
    viewport(eyeIndex);
    ProgramPtr distortProgram = GlUtils::getProgram(
      Resource::SHADERS_TEXTURED_VS,
      Resource::SHADERS_RIFTWARP_FS);
    distortProgram->use();
    distortProgram->setUniform1i("Scene", 1);
    distortProgram->setUniform1i("OffsetMap", 0);

    glActiveTexture(GL_TEXTURE1);
    sceneTextures[eyeIndex]->bind();
    glActiveTexture(GL_TEXTURE0);
    lookupTextures[eyeIndex]->bind();

    quadGeometry->bindVertexArray();
#ifdef DISTORTION_TIMING
    query->begin();
#endif
    quadGeometry->draw();

#ifdef DISTORTION_TIMING
    query->end();
    static long accumulator = 0;
    static long count = 0;
    accumulator += query->getReult();
    count++;
    SAY("%d ns", accumulator / count);
#endif

    VertexArray::unbind();
    Texture2d::unbind();
    Program::clear();
  }
};

RUN_OVR_APP(ShaderLookupDistort)
