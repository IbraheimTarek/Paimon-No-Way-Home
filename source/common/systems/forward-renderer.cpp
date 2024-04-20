#include <sstream>
#include "forward-renderer.hpp"
#include "../mesh/mesh-utils.hpp"
#include "../texture/texture-utils.hpp"
#include "../deserialize-utils.hpp"

namespace our {

    void ForwardRenderer::initialize(glm::ivec2 windowSize, const nlohmann::json& config){
        // First, we store the window size for later use
        this->windowSize = windowSize;
        this->areaLight = config.value("areaLight" , glm::vec3(1,1,1));
        // Then we check if there is a sky texture in the configuration
        if(config.contains("sky")){
            // First, we create a sphere which will be used to draw the sky
            this->skySphere = mesh_utils::sphere(glm::ivec2(16, 16));
            
            // We can draw the sky using the same shader used to draw textured objects
            ShaderProgram* skyShader = new ShaderProgram();
            //Fixme: change to texture if needed
            skyShader->attach("assets/shaders/default.vert", GL_VERTEX_SHADER);
            skyShader->attach("assets/shaders/default.frag", GL_FRAGMENT_SHADER);
            skyShader->link();
            
            //TODO: (Req 10) Pick the correct pipeline state to draw the sky
            // Hints: the sky will be draw after the opaque objects so we would need depth testing but which depth funtion should we pick?
            // We will draw the sphere from the inside, so what options should we pick for the face culling.
            PipelineState skyPipelineState{};
            skyPipelineState.faceCulling.enabled = true;
            skyPipelineState.faceCulling.frontFace = GL_CW;
            skyPipelineState.depthMask = true;
            skyPipelineState.depthTesting.enabled = true;

            // Load the sky texture (note that we don't need mipmaps since we want to avoid any unnecessary blurring while rendering the sky)
            auto skyTextureFile = config.value<std::string>("sky", "");
            Texture2D* skyTexture = texture_utils::loadImage(skyTextureFile, false);

            // Setup a sampler for the sky
            auto* skySampler = new Sampler();
            skySampler->set(GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            skySampler->set(GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            skySampler->set(GL_TEXTURE_WRAP_S, GL_REPEAT);
            skySampler->set(GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

            // Combine all the aforementioned objects (except the mesh) into a material 
            this->skyMaterial = new DefaultMaterial(); //Fixme change back to Textured Material if needed
            this->skyMaterial->shader = skyShader;
            this->skyMaterial->texture = skyTexture;
            this->skyMaterial->sampler = skySampler;
            this->skyMaterial->pipelineState = skyPipelineState;
            this->skyMaterial->tint = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
            this->skyMaterial->transparent = false;
            this->skyMaterial->isSkybox = true;
        }

        // Then we check if there is a postprocessing shader in the configuration
        if(config.contains("postprocess")){
            //TODO: (Req 11) Create a framebuffer
            glGenFramebuffers(1, &(this->postprocessFrameBuffer));
            glBindFramebuffer(GL_FRAMEBUFFER,this->postprocessFrameBuffer);
            //TODO: (Req 11) Create a color and a depth texture and attach them to the framebuffer
            // Hints: The color format can be (Red, Green, Blue and Alpha components with 8 bits for each channel).
            // The depth format can be (Depth component with 24 bits).
            colorTarget = texture_utils::empty(GL_RGBA8,windowSize);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTarget->getOpenGLName(), 0);
            depthTarget = texture_utils::empty(GL_DEPTH_COMPONENT24,windowSize);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depthTarget->getOpenGLName(), 0);
            //TODO: (Req 11) Unbind the framebuffer just to be safe
            glBindFramebuffer(GL_FRAMEBUFFER,0);
            // Create a vertex array to use for drawing the texture
            glGenVertexArrays(1, &postProcessVertexArray);

            // Create a sampler to use for sampling the scene texture in the post processing shader
            Sampler* postprocessSampler = new Sampler();
            postprocessSampler->set(GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            postprocessSampler->set(GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            postprocessSampler->set(GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            postprocessSampler->set(GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

            // Create the post processing shader
            ShaderProgram* postprocessShader = new ShaderProgram();
            postprocessShader->attach("assets/shaders/fullscreen.vert", GL_VERTEX_SHADER);
            postprocessShader->attach(config.value<std::string>("postprocess", ""), GL_FRAGMENT_SHADER);
            postprocessShader->link();

            // Create a post processing material
            postprocessMaterial = new TexturedMaterial();
            postprocessMaterial->shader = postprocessShader;
            postprocessMaterial->texture = colorTarget;
            postprocessMaterial->sampler = postprocessSampler;
            // The default options are fine but we don't need to interact with the depth buffer
            // so it is more performant to disable the depth mask
            postprocessMaterial->pipelineState.depthMask = false;
        }
    }

    void ForwardRenderer::destroy(){
        // Delete all objects related to the sky
        if(skyMaterial){
            delete skySphere;
            delete skyMaterial->shader;
            delete skyMaterial->texture;
            delete skyMaterial->sampler;
            delete skyMaterial;
        }
        // Delete all objects related to post processing
        if(postprocessMaterial){
            glDeleteFramebuffers(1, &postprocessFrameBuffer);
            glDeleteVertexArrays(1, &postProcessVertexArray);
            delete colorTarget;
            delete depthTarget;
            delete postprocessMaterial->sampler;
            delete postprocessMaterial->shader;
            delete postprocessMaterial;
        }
    }

    void ForwardRenderer::render(World* world){
        // First of all, we search for a camera and for all the mesh renderers
        CameraComponent* camera = nullptr;
        opaqueCommands.clear();
        transparentCommands.clear();
        directionalLights.clear();
        spotLights.clear();
        coneLights.clear();

        for(auto entity : world->getEntities()){
            // If we hadn't found a camera yet, we look for a camera in this entity
            if(!camera) camera = entity->getComponent<CameraComponent>();

            glm::mat4 localToWorld = entity->getLocalToWorldMatrix();
            glm::vec4 position = localToWorld * glm::vec4(0, 0, 0, 1);

            // If this entity has a mesh renderer component
            if(auto meshRenderer = entity->getComponent<MeshRendererComponent>(); meshRenderer){
                // We construct a command from it
                RenderCommand command;
                command.localToWorld = localToWorld;
                command.center = glm::vec3(position);
                command.mesh = meshRenderer->mesh;
                command.material = meshRenderer->material;
                // if it is transparent, we add it to the transparent commands list
                if(command.material->transparent){
                    transparentCommands.push_back(command);
                } else {
                // Otherwise, we add it to the opaque command list
                    opaqueCommands.push_back(command);
                }
            }

            auto dl = entity->getComponent<DirectionalLight>();
            if (dl != nullptr)
                directionalLights.emplace_back(dl);

            auto sl = entity->getComponent<SpotLight>();
            if (sl != nullptr) {
                spotLights.emplace_back(sl);
                sl->worldPosition = glm::vec3(position);
            }

            auto cl = entity->getAllComponents<ConeLight>();
            for (auto k : cl){
                coneLights.emplace_back(k);
                k->worldPosition = glm::vec3(position);
                k->worldDirection = glm::vec3(localToWorld * glm::vec4(k->direction , 0.0));
            }
        }

        // If there is no camera, we return (we cannot render without a camera)
        if(camera == nullptr) return;

        //TODO: (Req 9) Modify the following line such that "cameraForward" contains a vector pointing the camera forward direction
        // HINT: See how you wrote the CameraComponent::getViewMatrix, it should help you solve this one
        auto camTransform = camera->getOwner()->getLocalToWorldMatrix();
        glm::vec4 cameraForward_ =  camTransform * glm::vec4(0.0, 0.0, -1.0f , 0.0);
        glm::vec4 cameraCenter_  =  camTransform * glm::vec4(0.0, 0.0,  0.0f , 1.0);

        glm::vec3 cameraForward = glm::vec3(cameraForward_.x , cameraForward_.y , cameraForward_.z);
        glm::vec3 cameraCenter  = glm::vec3(cameraCenter_.x  , cameraCenter_.y  , cameraCenter_.z );

        std::sort(
                transparentCommands.begin(),
                transparentCommands.end(),
                [cameraForward,cameraCenter](const RenderCommand& first, const RenderCommand& second){
            //TODO: (Req 9) Finish this function
            // HINT: the following return should return true "first" should be drawn before "second".
            return glm::dot((second.center - cameraCenter) , cameraForward) <  glm::dot((first.center - cameraCenter) , cameraForward);
        });

        //TODO: (Req 9) Get the camera ViewProjection matrix and store it in VP
        auto VP = camera->getProjectionMatrix(this->windowSize) * camera->getViewMatrix();

        //TODO: (Req 9) Set the OpenGL viewport using viewportStart and viewportSize
        glViewport(0,0,windowSize.x , windowSize.y);

        //TODO: (Req 9) Set the clear color to black and the clear depth to 1
        glClearColor(0,0,0,0);
        glClearDepth(1);

        //TODO: (Req 9) Set the color mask to true and the depth mask to true (to ensure the glClear will affect the framebuffer)
        glColorMask(true , true , true , true);
        glDepthMask(true);


        // If there is a postprocess material, bind the framebuffer
        if(postprocessMaterial){
            //TODO: (Req 11) bind the framebuffer
            glBindFramebuffer(GL_FRAMEBUFFER,this->postprocessFrameBuffer);
        }

        //TODO: (Req 9) Clear the color and depth buffers
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        //TODO: (Req 9) Draw all the opaque commands
        // Don't forget to set the "transform" uniform to be equal the model-view-projection matrix for each render command
        for (auto k : opaqueCommands){
            k.material->setup();
            if (dynamic_cast<DefaultMaterial*>(k.material)){
                // set up transform
                k.material->shader->set("transform", k.localToWorld);
                k.material->shader->set("Camera", VP);
                k.material->shader->set("cameraPosition", cameraCenter);
                k.material->shader->set("areaLight" , areaLight);

                // set up lights
                k.material->shader->set("directionalLightCount" , (GLint) directionalLights.size());
                for (int i = 0;i < directionalLights.size();i++){
                    std::stringstream ss;
                    ss << "directionalLights[" << i << "].";
                    auto header = ss.str();
                    k.material->shader->set( header + "direction" , directionalLights[i]->direction);
                    k.material->shader->set( header + "intensity" , directionalLights[i]->intensity);
                    k.material->shader->set( header + "color" , directionalLights[i]->color);
                }

                k.material->shader->set("spotLightsCount" , (GLint) spotLights.size());
                for (int i = 0;i < spotLights.size();i++){
                    std::stringstream ss;
                    ss << "spotLights[" << i << "].";
                    auto header = ss.str();
                    k.material->shader->set( header + "position" , spotLights[i]->worldPosition);
                    k.material->shader->set( header + "intensity" , spotLights[i]->intensity);
                    k.material->shader->set( header + "color" , spotLights[i]->color);
                    k.material->shader->set( header + "decay" , spotLights[i]->lightDecay);
                }

                k.material->shader->set("coneLightsCount" , (GLint) coneLights.size());
                for (int i = 0;i < coneLights.size();i++){
                    std::stringstream ss;
                    ss << "coneLights[" << i << "].";
                    auto header = ss.str();
                    k.material->shader->set( header + "position" , coneLights[i]->worldPosition);
                    k.material->shader->set( header + "intensity" , coneLights[i]->intensity);
                    k.material->shader->set( header + "color" , coneLights[i]->color);
                    k.material->shader->set( header + "direction" , coneLights[i]->worldDirection);
                    k.material->shader->set( header + "range" , coneLights[i]->range);
                    k.material->shader->set( header + "smoothing" , coneLights[i]->smoothing);
                    k.material->shader->set( header + "decay" , coneLights[i]->lightDecay);
                }
            }else{
                k.material->shader->set("transform", VP * k.localToWorld);
            }
            k.mesh->draw();
        }

        // If there is a sky material, draw the sky
        if(this->skyMaterial){
            //TODO: (Req 10) setup the sky material
            skyMaterial->setup();
            skyMaterial->shader->set("areaLight" , areaLight);

            //TODO: (Req 10) Get the camera position
            //...

            //TODO: (Req 10) Create a model matrix for the sy such that it always follows the camera (sky sphere center = camera position)
            auto M = glm::translate(glm::mat4(1.0f) , cameraCenter);

            //TODO: (Req 10) We want the sky to be drawn behind everything (in NDC space, z=1)
            // We can achieve the is by multiplying by an extra matrix after the projection but what values should we put in it?
            glm::mat4 alwaysBehindTransform = glm::mat4(
                1.0f, 0.0f, 0.0f, 0.0f,
                0.0f, 1.0f, 0.0f, 0.0f,
                0.0f, 0.0f, 0.0f, 0.0f,
                0.0f, 0.0f, 1.0f, 1.0f
            ); //this thing gets transposed ...

            // Create a scale matrix for the skybox
            glm::mat4 skyboxScaleMatrix = glm::scale(glm::mat4(1.0f), glm::vec3(camera->orthoHeight * 2, camera->orthoHeight * 2, camera->orthoHeight * 2));

            //TODO: (Req 10) set the "transform" uniform
            skyMaterial->shader->set("transform", M * skyboxScaleMatrix);
            skyMaterial->shader->set("Camera", alwaysBehindTransform * VP);

            //TODO: (Req 10) draw the sky sphere
            skySphere->draw();
        }
        //TODO: (Req 9) Draw all the transparent commands
        // Don't forget to set the "transform" uniform to be equal the model-view-projection matrix for each render command
        for (auto k : transparentCommands){
            k.material->setup();
            if (dynamic_cast<DefaultMaterial*>(k.material)){
                // set up transform
                k.material->shader->set("transform", k.localToWorld);
                k.material->shader->set("Camera", VP);
                k.material->shader->set("areaLight" , areaLight);
                k.material->shader->set("cameraPosition", cameraCenter);

                // set up lights
                k.material->shader->set("directionalLightCount" , (GLint) directionalLights.size());
                for (int i = 0;i < directionalLights.size();i++){
                    std::stringstream ss;
                    ss << "directionalLights[" << i << "].";
                    auto header = ss.str();
                    k.material->shader->set( header + "direction" , directionalLights[i]->direction);
                    k.material->shader->set( header + "intensity" , directionalLights[i]->intensity);
                    k.material->shader->set( header + "color" , directionalLights[i]->color);
                }

                k.material->shader->set("spotLightsCount" , (GLint) spotLights.size());
                for (int i = 0;i < spotLights.size();i++){
                    std::stringstream ss;
                    ss << "spotLights[" << i << "].";
                    auto header = ss.str();
                    k.material->shader->set( header + "position" , spotLights[i]->worldPosition);
                    k.material->shader->set( header + "intensity" , spotLights[i]->intensity);
                    k.material->shader->set( header + "color" , spotLights[i]->color);
                    k.material->shader->set( header + "decay" , spotLights[i]->lightDecay);
                }

                k.material->shader->set("coneLightsCount" , (GLint) coneLights.size());
                for (int i = 0;i < coneLights.size();i++){
                    std::stringstream ss;
                    ss << "coneLights[" << i << "].";
                    auto header = ss.str();
                    k.material->shader->set( header + "position" , coneLights[i]->worldPosition);
                    k.material->shader->set( header + "intensity" , coneLights[i]->intensity);
                    k.material->shader->set( header + "color" , coneLights[i]->color);
                    k.material->shader->set( header + "direction" , coneLights[i]->worldDirection);
                    k.material->shader->set( header + "range" , coneLights[i]->range);
                    k.material->shader->set( header + "smoothing" , coneLights[i]->smoothing);
                    k.material->shader->set( header + "decay" , coneLights[i]->lightDecay);
                }
            }else{
                k.material->shader->set("transform", VP * k.localToWorld);
            }
            k.mesh->draw();
        }

        // If there is a postprocess material, apply postprocessing
        if(postprocessMaterial){
            //TODO: (Req 11) Return to the default framebuffer
            glBindFramebuffer(GL_FRAMEBUFFER,0);
            //TODO: (Req 11) Setup the postprocess material and draw the fullscreen triangle
            postprocessMaterial->setup();
            glBindVertexArray(postProcessVertexArray);
            glDrawArrays(GL_TRIANGLES,0,3);
        }
    }

}