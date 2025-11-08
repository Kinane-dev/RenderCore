// runtime_player.cpp
// Version complète du moteur pour exécuter les jeux exportés
// Compiler ce fichier séparément en "RenderCore_Runtime.exe"

#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include <map>
#include <fstream>

#include <SDL.h>
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include <nlohmann/json.hpp>

#ifndef NO_LUA
#include <lua.hpp>
#endif

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

std::string g_currentScriptParent = "";

using json = nlohmann::json;

// ===== FORWARD DECLARATIONS =====
#ifndef NO_LUA
extern lua_State* g_luaState;  // Déclaration externe
#endif

// ===== STRUCTURES =====
struct Camera {
    glm::vec3 position = glm::vec3(5.0f, 5.0f, 5.0f);
    glm::vec3 front = glm::vec3(0.0f, 0.0f, -1.0f);
    glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f);
    glm::vec3 right = glm::vec3(1.0f, 0.0f, 0.0f);
    float yaw = -90.0f;
    float pitch = 0.0f;
    float fov = 45.0f;
    float nearPlane = 0.1f;
    float farPlane = 100.0f;
    
    void updateVectors() {
        glm::vec3 direction;
        direction.x = cos(glm::radians(yaw)) * cos(glm::radians(pitch));
        direction.y = sin(glm::radians(pitch));
        direction.z = sin(glm::radians(yaw)) * cos(glm::radians(pitch));
        front = glm::normalize(direction);
        right = glm::normalize(glm::cross(front, glm::vec3(0.0f, 1.0f, 0.0f)));
        up = glm::normalize(glm::cross(right, front));
    }
    
    glm::mat4 getViewMatrix() {
        return glm::lookAt(position, position + front, up);
    }
    
    glm::mat4 getProjectionMatrix(float aspectRatio) {
        return glm::perspective(glm::radians(fov), aspectRatio, nearPlane, farPlane);
    }
};

struct InputState {
    bool keys[SDL_NUM_SCANCODES] = {false};
    bool mouseButtons[8] = {false};
    int mouseX = 0, mouseY = 0;
};

enum class ObjectType {
    CUBE, SPHERE, PLANE, LIGHT, CAMERA
};

struct SceneObject {
    std::string name;
    std::string textureName;
    std::string texturePath;
    std::string modelPath;
    std::string scriptPath;
    
    glm::mat4 transform = glm::mat4(1.0f);
    ObjectType type = ObjectType::CUBE;
    glm::vec3 color = glm::vec3(1.0f);
    bool visible = true;
    
    GLuint VAO = 0;
    GLuint VBO = 0;
    GLuint EBO = 0;
    int indexCount = 0;
    
    bool hasPhysics = false;
    bool isStatic = false;
    glm::vec3 velocity = glm::vec3(0.0f);
    
    glm::vec3 scale = glm::vec3(1.0f);
    glm::vec3 rotation = glm::vec3(0.0f);
    glm::vec3 position = glm::vec3(0.0f);
    
    float lightIntensity = 1.0f;
    glm::vec3 lightColor = glm::vec3(1.0f);
    float lightRadius = 10.0f;
    
    float cameraFOV = 45.0f;
    float cameraNear = 0.1f;
    float cameraFar = 100.0f;
    
    int parentIndex = -1;
    std::vector<int> childIndices;
    
    std::vector<std::string> animationNames;
    std::string currentAnimation = "None";
    bool isAnimationPlaying = false;
    float animationTime = 0.0f;
    const aiScene* scene = nullptr;
    Assimp::Importer* importer = nullptr;
    
    void updateTransform() {
        glm::mat4 T = glm::translate(glm::mat4(1.0f), position);
        glm::mat4 R = glm::rotate(glm::mat4(1.0f), glm::radians(rotation.x), glm::vec3(1, 0, 0));
        R = glm::rotate(R, glm::radians(rotation.y), glm::vec3(0, 1, 0));
        R = glm::rotate(R, glm::radians(rotation.z), glm::vec3(0, 0, 1));
        glm::mat4 S = glm::scale(glm::mat4(1.0f), scale);
        transform = T * R * S;
    }
    
    void runScript();  // Déclaration seulement
    
    ~SceneObject() {
        if (VAO != 0) glDeleteVertexArrays(1, &VAO);
        if (VBO != 0) glDeleteBuffers(1, &VBO);
        if (EBO != 0) glDeleteBuffers(1, &EBO);
        if (importer) delete importer;
    }
};

// ===== VARIABLES GLOBALES =====
SDL_Window* g_window = nullptr;
SDL_GLContext g_glContext = nullptr;
#ifndef NO_LUA
lua_State* g_luaState = nullptr;  // Définition ici
#endif
GLuint g_shaderProgram = 0;
GLuint g_skyboxShader = 0;
std::vector<std::unique_ptr<SceneObject>> g_sceneObjects;
std::map<std::string, GLuint> g_textures;
Camera g_camera;
InputState g_input;
int g_sceneCamera = -1;
glm::vec3 g_skyboxTopColor = glm::vec3(0.2f, 0.3f, 0.6f);
glm::vec3 g_skyboxBottomColor = glm::vec3(0.6f, 0.7f, 0.9f);
GLuint g_skyboxVAO = 0;
GLuint g_skyboxVBO = 0;
float g_deltaTime = 0.0f;

struct GameConfig {
    std::string gameName = "Game";
    bool fullscreen = false;
    int windowWidth = 1280;
    int windowHeight = 720;
    std::string sceneFile = "data/game.scene";
} g_gameConfig;

// ===== IMPLÉMENTATION DE runScript() =====
void SceneObject::runScript() {
#ifndef NO_LUA
    if (scriptPath.empty() || g_luaState == nullptr) return;

    g_currentScriptParent = name;

    if (luaL_dofile(g_luaState, scriptPath.c_str()) != LUA_OK) {
        std::cout << "[LUA ERROR] Script error (" << name << "): " 
                  << lua_tostring(g_luaState, -1) << std::endl;
        lua_pop(g_luaState, 1);
    } else {
        lua_getglobal(g_luaState, "OnUpdate");
        if (lua_isfunction(g_luaState, -1)) {
            if (lua_pcall(g_luaState, 0, 0, 0) != LUA_OK) {
                std::cout << "[LUA ERROR] OnUpdate (" << name << "): " 
                          << lua_tostring(g_luaState, -1) << std::endl;
                lua_pop(g_luaState, 1);
            }
        } else {
            lua_pop(g_luaState, 1);
        }
    }

    g_currentScriptParent = "";
#endif
}

// ===== FONCTIONS LUA =====
#ifndef NO_LUA

int Lua_GetObjectByName(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    for (size_t i = 0; i < g_sceneObjects.size(); ++i) {
        if (g_sceneObjects[i]->name == name) {
            lua_pushinteger(L, i);
            return 1;
        }
    }
    lua_pushnil(L);
    return 1;
}

int Lua_GetSelf(lua_State* L) {
    if (g_currentScriptParent.empty()) {
        lua_pushnil(L);
        return 1;
    }
    for (size_t i = 0; i < g_sceneObjects.size(); ++i) {
        if (g_sceneObjects[i]->name == g_currentScriptParent) {
            lua_pushinteger(L, i);
            return 1;
        }
    }
    lua_pushnil(L);
    return 1;
}

int Lua_SetPosition(lua_State* L) {
    int objID = luaL_checkinteger(L, 1);
    if (objID < 0 || objID >= static_cast<int>(g_sceneObjects.size())) return 0;
    float x = luaL_checknumber(L, 2);
    float y = luaL_checknumber(L, 3);
    float z = luaL_checknumber(L, 4);
    g_sceneObjects[objID]->position = glm::vec3(x, y, z);
    g_sceneObjects[objID]->updateTransform();
    return 0;
}

int Lua_GetPosition(lua_State* L) {
    int objID = luaL_checkinteger(L, 1);
    if (objID < 0 || objID >= static_cast<int>(g_sceneObjects.size())) {
        lua_pushnil(L);
        return 1;
    }
    auto& pos = g_sceneObjects[objID]->position;
    lua_pushnumber(L, pos.x);
    lua_pushnumber(L, pos.y);
    lua_pushnumber(L, pos.z);
    return 3;
}

int Lua_Move(lua_State* L) {
    int objID = luaL_checkinteger(L, 1);
    if (objID < 0 || objID >= static_cast<int>(g_sceneObjects.size())) return 0;
    float dx = luaL_checknumber(L, 2);
    float dy = luaL_checknumber(L, 3);
    float dz = luaL_checknumber(L, 4);
    g_sceneObjects[objID]->position += glm::vec3(dx, dy, dz);
    g_sceneObjects[objID]->updateTransform();
    return 0;
}

int Lua_SetRotation(lua_State* L) {
    int objID = luaL_checkinteger(L, 1);
    if (objID < 0 || objID >= static_cast<int>(g_sceneObjects.size())) return 0;
    float x = luaL_checknumber(L, 2);
    float y = luaL_checknumber(L, 3);
    float z = luaL_checknumber(L, 4);
    g_sceneObjects[objID]->rotation = glm::vec3(x, y, z);
    g_sceneObjects[objID]->updateTransform();
    return 0;
}

int Lua_GetRotation(lua_State* L) {
    int objID = luaL_checkinteger(L, 1);
    if (objID < 0 || objID >= static_cast<int>(g_sceneObjects.size())) {
        lua_pushnil(L);
        return 1;
    }
    auto& rot = g_sceneObjects[objID]->rotation;
    lua_pushnumber(L, rot.x);
    lua_pushnumber(L, rot.y);
    lua_pushnumber(L, rot.z);
    return 3;
}

int Lua_Rotate(lua_State* L) {
    int objID = luaL_checkinteger(L, 1);
    if (objID < 0 || objID >= static_cast<int>(g_sceneObjects.size())) return 0;
    float dx = luaL_checknumber(L, 2);
    float dy = luaL_checknumber(L, 3);
    float dz = luaL_checknumber(L, 4);
    g_sceneObjects[objID]->rotation += glm::vec3(dx, dy, dz);
    g_sceneObjects[objID]->updateTransform();
    return 0;
}

int Lua_SetScale(lua_State* L) {
    int objID = luaL_checkinteger(L, 1);
    if (objID < 0 || objID >= static_cast<int>(g_sceneObjects.size())) return 0;
    float x = luaL_checknumber(L, 2);
    float y = luaL_checknumber(L, 3);
    float z = luaL_checknumber(L, 4);
    g_sceneObjects[objID]->scale = glm::vec3(x, y, z);
    g_sceneObjects[objID]->updateTransform();
    return 0;
}

int Lua_GetScale(lua_State* L) {
    int objID = luaL_checkinteger(L, 1);
    if (objID < 0 || objID >= static_cast<int>(g_sceneObjects.size())) {
        lua_pushnil(L);
        return 1;
    }
    auto& scale = g_sceneObjects[objID]->scale;
    lua_pushnumber(L, scale.x);
    lua_pushnumber(L, scale.y);
    lua_pushnumber(L, scale.z);
    return 3;
}

int Lua_SetVelocity(lua_State* L) {
    int objID = luaL_checkinteger(L, 1);
    if (objID < 0 || objID >= static_cast<int>(g_sceneObjects.size())) return 0;
    float x = luaL_checknumber(L, 2);
    float y = luaL_checknumber(L, 3);
    float z = luaL_checknumber(L, 4);
    g_sceneObjects[objID]->velocity = glm::vec3(x, y, z);
    return 0;
}

int Lua_GetVelocity(lua_State* L) {
    int objID = luaL_checkinteger(L, 1);
    if (objID < 0 || objID >= static_cast<int>(g_sceneObjects.size())) {
        lua_pushnil(L);
        return 1;
    }
    auto& vel = g_sceneObjects[objID]->velocity;
    lua_pushnumber(L, vel.x);
    lua_pushnumber(L, vel.y);
    lua_pushnumber(L, vel.z);
    return 3;
}

int Lua_AddVelocity(lua_State* L) {
    int objID = luaL_checkinteger(L, 1);
    if (objID < 0 || objID >= static_cast<int>(g_sceneObjects.size())) return 0;
    float x = luaL_checknumber(L, 2);
    float y = luaL_checknumber(L, 3);
    float z = luaL_checknumber(L, 4);
    g_sceneObjects[objID]->velocity += glm::vec3(x, y, z);
    return 0;
}

int Lua_SetVisible(lua_State* L) {
    int objID = luaL_checkinteger(L, 1);
    if (objID < 0 || objID >= static_cast<int>(g_sceneObjects.size())) return 0;
    bool visible = lua_toboolean(L, 2);
    g_sceneObjects[objID]->visible = visible;
    return 0;
}

int Lua_IsVisible(lua_State* L) {
    int objID = luaL_checkinteger(L, 1);
    if (objID < 0 || objID >= static_cast<int>(g_sceneObjects.size())) {
        lua_pushboolean(L, false);
        return 1;
    }
    lua_pushboolean(L, g_sceneObjects[objID]->visible);
    return 1;
}

int Lua_SetColor(lua_State* L) {
    int objID = luaL_checkinteger(L, 1);
    if (objID < 0 || objID >= static_cast<int>(g_sceneObjects.size())) return 0;
    float r = luaL_checknumber(L, 2);
    float g = luaL_checknumber(L, 3);
    float b = luaL_checknumber(L, 4);
    g_sceneObjects[objID]->color = glm::vec3(r, g, b);
    return 0;
}

int Lua_GetColor(lua_State* L) {
    int objID = luaL_checkinteger(L, 1);
    if (objID < 0 || objID >= static_cast<int>(g_sceneObjects.size())) {
        lua_pushnil(L);
        return 1;
    }
    auto& col = g_sceneObjects[objID]->color;
    lua_pushnumber(L, col.r);
    lua_pushnumber(L, col.g);
    lua_pushnumber(L, col.b);
    return 3;
}

int Lua_IsKeyPressed(lua_State* L) {
    const char* keyName = luaL_checkstring(L, 1);
    static const std::map<std::string, SDL_Scancode> keyMap = {
        {"W", SDL_SCANCODE_W}, {"A", SDL_SCANCODE_A},
        {"S", SDL_SCANCODE_S}, {"D", SDL_SCANCODE_D},
        {"Q", SDL_SCANCODE_Q}, {"E", SDL_SCANCODE_E},
        {"Space", SDL_SCANCODE_SPACE}, {"Shift", SDL_SCANCODE_LSHIFT},
        {"Ctrl", SDL_SCANCODE_LCTRL}, {"Alt", SDL_SCANCODE_LALT},
        {"Up", SDL_SCANCODE_UP}, {"Down", SDL_SCANCODE_DOWN},
        {"Left", SDL_SCANCODE_LEFT}, {"Right", SDL_SCANCODE_RIGHT},
        {"Enter", SDL_SCANCODE_RETURN}, {"Escape", SDL_SCANCODE_ESCAPE}
    };
    auto it = keyMap.find(keyName);
    if (it != keyMap.end()) {
        lua_pushboolean(L, g_input.keys[it->second]);
    } else {
        lua_pushboolean(L, false);
    }
    return 1;
}

int Lua_GetDeltaTime(lua_State* L) {
    lua_pushnumber(L, g_deltaTime);
    return 1;
}

int Lua_GetDistance(lua_State* L) {
    int obj1 = luaL_checkinteger(L, 1);
    int obj2 = luaL_checkinteger(L, 2);
    if (obj1 < 0 || obj1 >= static_cast<int>(g_sceneObjects.size()) ||
        obj2 < 0 || obj2 >= static_cast<int>(g_sceneObjects.size())) {
        lua_pushnumber(L, -1.0);
        return 1;
    }
    glm::vec3 diff = g_sceneObjects[obj1]->position - g_sceneObjects[obj2]->position;
    lua_pushnumber(L, glm::length(diff));
    return 1;
}

int Lua_Print(lua_State* L) {
    const char* message = luaL_checkstring(L, 1);
    std::cout << "[LUA] " << message << std::endl;
    return 0;
}

int Lua_PlayAnimation(lua_State* L) {
    int objID = luaL_checkinteger(L, 1);
    if (objID < 0 || objID >= static_cast<int>(g_sceneObjects.size())) return 0;
    const char* animName = luaL_checkstring(L, 2);
    auto& obj = g_sceneObjects[objID];
    obj->currentAnimation = animName;
    obj->isAnimationPlaying = true;
    obj->animationTime = 0.0f;
    return 0;
}

int Lua_StopAnimation(lua_State* L) {
    int objID = luaL_checkinteger(L, 1);
    if (objID < 0 || objID >= static_cast<int>(g_sceneObjects.size())) return 0;
    g_sceneObjects[objID]->isAnimationPlaying = false;
    return 0;
}

#endif

// ===== FONCTIONS UTILITAIRES =====
void Log(const std::string& msg) {
    std::cout << "[RUNTIME] " << msg << std::endl;
}

GLuint LoadTexture(const std::string& path) {
    GLuint textureID;
    glGenTextures(1, &textureID);
    
    int width, height, nrChannels;
    unsigned char* data = stbi_load(path.c_str(), &width, &height, &nrChannels, 0);
    if (data) {
        GLenum format = (nrChannels == 4) ? GL_RGBA : GL_RGB;
        glBindTexture(GL_TEXTURE_2D, textureID);
        glTexImage2D(GL_TEXTURE_2D, 0, format, width, height, 0, format, GL_UNSIGNED_BYTE, data);
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        stbi_image_free(data);
        Log("Texture loaded: " + path);
    } else {
        Log("Failed to load texture: " + path);
        glDeleteTextures(1, &textureID);
        return 0;
    }
    return textureID;
}

// ===== SHADERS =====
const char* vertexShaderSource = R"(
#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec2 aTexCoord;
layout (location = 2) in vec3 aNormal;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;

out vec2 TexCoord;
out vec3 FragPos;
out vec3 Normal;

void main()
{
    gl_Position = projection * view * model * vec4(aPos, 1.0);
    FragPos = vec3(model * vec4(aPos, 1.0));
    Normal = mat3(transpose(inverse(model))) * aNormal;
    TexCoord = aTexCoord;
}
)";

const char* fragmentShaderSource = R"(
#version 330 core
out vec4 FragColor;

in vec2 TexCoord;
in vec3 FragPos;
in vec3 Normal;

uniform vec3 objectColor;
uniform bool hasTexture;
uniform sampler2D texture1;
uniform int numLights;
uniform vec3 lightPositions[8];
uniform vec3 lightColors[8];
uniform float lightIntensities[8];
uniform float lightRadii[8];
uniform vec3 viewPos;

void main()
{
    vec3 color = objectColor;
    
    if (hasTexture) {
        vec3 texColor = texture(texture1, TexCoord).rgb;
        color = color * texColor;
    }
    
    vec3 normal = normalize(Normal);
    vec3 lighting = vec3(0.2);
    
    for(int i = 0; i < numLights && i < 8; i++) {
        vec3 lightDir = lightPositions[i] - FragPos;
        float distance = length(lightDir);
        
        if(distance < lightRadii[i]) {
            lightDir = normalize(lightDir);
            float diff = max(dot(normal, lightDir), 0.0);
            float attenuation = 1.0 - (distance / lightRadii[i]);
            attenuation = attenuation * attenuation;
            vec3 diffuse = diff * lightColors[i] * lightIntensities[i] * attenuation;
            lighting += diffuse;
        }
    }
    
    color = color * lighting;
    FragColor = vec4(color, 1.0);
}
)";

const char* skyboxVertexShader = R"(
#version 330 core
layout (location = 0) in vec3 aPos;
uniform mat4 view;
uniform mat4 projection;
out vec3 WorldPos;

void main()
{
    WorldPos = aPos;
    mat4 rotView = mat4(mat3(view));
    vec4 clipPos = projection * rotView * vec4(aPos, 1.0);
    gl_Position = clipPos.xyww;
}
)";

const char* skyboxFragmentShader = R"(
#version 330 core
out vec4 FragColor;
in vec3 WorldPos;
uniform vec3 topColor;
uniform vec3 bottomColor;

void main()
{
    float t = (normalize(WorldPos).y + 1.0) * 0.5;
    vec3 color = mix(bottomColor, topColor, t);
    FragColor = vec4(color, 1.0);
}
)";

GLuint CompileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    
    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetShaderInfoLog(shader, 512, NULL, infoLog);
        Log("Shader compilation error: " + std::string(infoLog));
    }
    return shader;
}

void InitOpenGL() {
    // ===== COMPILATION DES SHADERS PRINCIPAUX =====
    GLuint vertexShader = CompileShader(GL_VERTEX_SHADER, vertexShaderSource);
    GLuint fragmentShader = CompileShader(GL_FRAGMENT_SHADER, fragmentShaderSource);
    
    g_shaderProgram = glCreateProgram();
    glAttachShader(g_shaderProgram, vertexShader);
    glAttachShader(g_shaderProgram, fragmentShader);
    glLinkProgram(g_shaderProgram);
    
    GLint success;
    glGetProgramiv(g_shaderProgram, GL_LINK_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetProgramInfoLog(g_shaderProgram, 512, NULL, infoLog);
        Log("Program link error: " + std::string(infoLog));
    }
    
    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);
    
    // ===== COMPILATION DES SHADERS SKYBOX =====
    GLuint skyVertexShader = CompileShader(GL_VERTEX_SHADER, skyboxVertexShader);
    GLuint skyFragmentShader = CompileShader(GL_FRAGMENT_SHADER, skyboxFragmentShader);
    
    g_skyboxShader = glCreateProgram();
    glAttachShader(g_skyboxShader, skyVertexShader);
    glAttachShader(g_skyboxShader, skyFragmentShader);
    glLinkProgram(g_skyboxShader);
    
    glGetProgramiv(g_skyboxShader, GL_LINK_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetProgramInfoLog(g_skyboxShader, 512, NULL, infoLog);
        Log("Skybox program link error: " + std::string(infoLog));
    }
    
    glDeleteShader(skyVertexShader);
    glDeleteShader(skyFragmentShader);
    
    // ===== CRÉATION DE LA GÉOMÉTRIE SKYBOX =====
    float skyboxVertices[] = {
        // Face arrière
        -1.0f,  1.0f, -1.0f,
        -1.0f, -1.0f, -1.0f,
         1.0f, -1.0f, -1.0f,
         1.0f, -1.0f, -1.0f,
         1.0f,  1.0f, -1.0f,
        -1.0f,  1.0f, -1.0f,

        // Face gauche
        -1.0f, -1.0f,  1.0f,
        -1.0f, -1.0f, -1.0f,
        -1.0f,  1.0f, -1.0f,
        -1.0f,  1.0f, -1.0f,
        -1.0f,  1.0f,  1.0f,
        -1.0f, -1.0f,  1.0f,

        // Face droite
         1.0f, -1.0f, -1.0f,
         1.0f, -1.0f,  1.0f,
         1.0f,  1.0f,  1.0f,
         1.0f,  1.0f,  1.0f,
         1.0f,  1.0f, -1.0f,
         1.0f, -1.0f, -1.0f,

        // Face avant
        -1.0f, -1.0f,  1.0f,
        -1.0f,  1.0f,  1.0f,
         1.0f,  1.0f,  1.0f,
         1.0f,  1.0f,  1.0f,
         1.0f, -1.0f,  1.0f,
        -1.0f, -1.0f,  1.0f,

        // Face haut
        -1.0f,  1.0f, -1.0f,
         1.0f,  1.0f, -1.0f,
         1.0f,  1.0f,  1.0f,
         1.0f,  1.0f,  1.0f,
        -1.0f,  1.0f,  1.0f,
        -1.0f,  1.0f, -1.0f,

        // Face bas
        -1.0f, -1.0f, -1.0f,
        -1.0f, -1.0f,  1.0f,
         1.0f, -1.0f, -1.0f,
         1.0f, -1.0f, -1.0f,
        -1.0f, -1.0f,  1.0f,
         1.0f, -1.0f,  1.0f
    };
    
    glGenVertexArrays(1, &g_skyboxVAO);
    glGenBuffers(1, &g_skyboxVBO);
    glBindVertexArray(g_skyboxVAO);
    glBindBuffer(GL_ARRAY_BUFFER, g_skyboxVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(skyboxVertices), skyboxVertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glBindVertexArray(0);
    
    Log("OpenGL initialized successfully");
}

void LoadMeshFromFile(SceneObject* obj, const std::string& path) {
    obj->importer = new Assimp::Importer();
    const aiScene* scene = obj->importer->ReadFile(path,
        aiProcess_Triangulate | aiProcess_GenNormals | aiProcess_JoinIdenticalVertices);

    if (!scene || !scene->mRootNode || scene->mNumMeshes == 0) {
        Log("Assimp error: " + std::string(obj->importer->GetErrorString()));
        delete obj->importer;
        obj->importer = nullptr;
        return;
    }

    obj->scene = scene;
    aiMesh* mesh = scene->mMeshes[0];

    std::vector<float> vertices;
    std::vector<unsigned int> indices;

    for (unsigned int i = 0; i < mesh->mNumVertices; i++) {
        vertices.push_back(mesh->mVertices[i].x);
        vertices.push_back(mesh->mVertices[i].y);
        vertices.push_back(mesh->mVertices[i].z);

        if (mesh->mTextureCoords[0]) {
            vertices.push_back(mesh->mTextureCoords[0][i].x);
            vertices.push_back(1.0f - mesh->mTextureCoords[0][i].y);
        } else {
            vertices.push_back(0.0f);
            vertices.push_back(0.0f);
        }

        if (mesh->mNormals) {
            vertices.push_back(mesh->mNormals[i].x);
            vertices.push_back(mesh->mNormals[i].y);
            vertices.push_back(mesh->mNormals[i].z);
        } else {
            vertices.push_back(0.0f);
            vertices.push_back(1.0f);
            vertices.push_back(0.0f);
        }
    }

    for (unsigned int i = 0; i < mesh->mNumFaces; i++) {
        aiFace face = mesh->mFaces[i];
        for (unsigned int j = 0; j < face.mNumIndices; j++) {
            indices.push_back(face.mIndices[j]);
        }
    }

    obj->indexCount = static_cast<int>(indices.size());

    glGenVertexArrays(1, &obj->VAO);
    glGenBuffers(1, &obj->VBO);
    glGenBuffers(1, &obj->EBO);

    glBindVertexArray(obj->VAO);
    glBindBuffer(GL_ARRAY_BUFFER, obj->VBO);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(float), vertices.data(), GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, obj->EBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(unsigned int), indices.data(), GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(5 * sizeof(float)));
    glEnableVertexAttribArray(2);

    glBindVertexArray(0);
    Log("Mesh loaded: " + path);
}

void InitLua() {
#ifndef NO_LUA
    g_luaState = luaL_newstate();
    luaL_openlibs(g_luaState);
    
    lua_register(g_luaState, "GetObjectByName", Lua_GetObjectByName);
    lua_register(g_luaState, "GetSelf", Lua_GetSelf);
    lua_register(g_luaState, "SetPosition", Lua_SetPosition);
    lua_register(g_luaState, "GetPosition", Lua_GetPosition);
    lua_register(g_luaState, "Move", Lua_Move);
    lua_register(g_luaState, "SetRotation", Lua_SetRotation);
    lua_register(g_luaState, "GetRotation", Lua_GetRotation);
    lua_register(g_luaState, "Rotate", Lua_Rotate);
    lua_register(g_luaState, "SetScale", Lua_SetScale);
    lua_register(g_luaState, "GetScale", Lua_GetScale);
    lua_register(g_luaState, "SetVelocity", Lua_SetVelocity);
    lua_register(g_luaState, "GetVelocity", Lua_GetVelocity);
    lua_register(g_luaState, "AddVelocity", Lua_AddVelocity);
    lua_register(g_luaState, "SetVisible", Lua_SetVisible);
    lua_register(g_luaState, "IsVisible", Lua_IsVisible);
    lua_register(g_luaState, "SetColor", Lua_SetColor);
    lua_register(g_luaState, "GetColor", Lua_GetColor);
    lua_register(g_luaState, "IsKeyPressed", Lua_IsKeyPressed);
    lua_register(g_luaState, "GetDeltaTime", Lua_GetDeltaTime);
    lua_register(g_luaState, "GetDistance", Lua_GetDistance);
    lua_register(g_luaState, "Print", Lua_Print);
    lua_register(g_luaState, "PlayAnimation", Lua_PlayAnimation);
    lua_register(g_luaState, "StopAnimation", Lua_StopAnimation);
    
    Log("Lua initialized with all functions");
#endif
}

void LoadScene(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        Log("ERROR: Cannot load scene: " + path);
        return;
    }
    
    json sceneJson;
    file >> sceneJson;
    file.close();
    
    g_sceneObjects.clear();
    
    for (auto& objJson : sceneJson["objects"]) {
        auto obj = std::make_unique<SceneObject>();
        obj->name = objJson["name"];
        obj->type = (ObjectType)(int)objJson["type"];
        obj->position = glm::vec3(objJson["position"][0], objJson["position"][1], objJson["position"][2]);
        obj->rotation = glm::vec3(objJson["rotation"][0], objJson["rotation"][1], objJson["rotation"][2]);
        obj->scale = glm::vec3(objJson["scale"][0], objJson["scale"][1], objJson["scale"][2]);
        obj->color = glm::vec3(objJson["color"][0], objJson["color"][1], objJson["color"][2]);
        obj->textureName = objJson.value("textureName", "");
        obj->texturePath = objJson.value("texturePath", "");
        obj->modelPath = objJson.value("modelPath", "");
        obj->scriptPath = objJson.value("script", "");
        obj->hasPhysics = objJson.value("hasPhysics", false);
        obj->isStatic = objJson.value("isStatic", false);
        
        obj->lightIntensity = objJson.value("lightIntensity", 1.0f);
        if (objJson.contains("lightColor")) {
            obj->lightColor = glm::vec3(objJson["lightColor"][0], objJson["lightColor"][1], objJson["lightColor"][2]);
        }
        obj->lightRadius = objJson.value("lightRadius", 10.0f);
        
        obj->cameraFOV = objJson.value("cameraFOV", 45.0f);
        obj->cameraNear = objJson.value("cameraNear", 0.1f);
        obj->cameraFar = objJson.value("cameraFar", 100.0f);
        
        if (!obj->textureName.empty() && g_textures.find(obj->textureName) == g_textures.end()) {
            if (!obj->texturePath.empty()) {
                GLuint texID = LoadTexture(obj->texturePath);
                if (texID != 0) {
                    g_textures[obj->textureName] = texID;
                }
            }
        }
        
        if (!obj->modelPath.empty()) {
            LoadMeshFromFile(obj.get(), obj->modelPath);
        }
        
        obj->updateTransform();
        
        if (obj->type == ObjectType::CAMERA) {
            g_sceneCamera = g_sceneObjects.size();
        }
        
        g_sceneObjects.push_back(std::move(obj));
    }
    
    if (sceneJson.contains("sceneCamera")) {
        g_sceneCamera = sceneJson["sceneCamera"];
    }
    
    Log("Scene loaded: " + path + " (" + std::to_string(g_sceneObjects.size()) + " objects)");
}

void LoadGameConfig() {
    std::ifstream file("data/game.config");
    if (!file.is_open()) {
        Log("Warning: game.config not found, using defaults");
        return;
    }
    
    json config;
    file >> config;
    file.close();
    
    g_gameConfig.gameName = config.value("gameName", "Game");
    g_gameConfig.fullscreen = config.value("fullscreen", false);
    g_gameConfig.windowWidth = config.value("windowWidth", 1280);
    g_gameConfig.windowHeight = config.value("windowHeight", 720);
    g_gameConfig.sceneFile = config.value("sceneFile", "data/game.scene");
    
    Log("Game config loaded: " + g_gameConfig.gameName);
}

void ProcessInput(float deltaTime) {
    // Input is handled by scripts in runtime mode
}

void UpdatePhysics(float deltaTime) {
    const glm::vec3 gravity(0.0f, -9.81f, 0.0f);
    
    for (auto& obj : g_sceneObjects) {
        if (!obj->hasPhysics || obj->isStatic) continue;
        
        obj->velocity += gravity * deltaTime;
        obj->position += obj->velocity * deltaTime;
        
        if (obj->position.y < -1.0f) {
            obj->position.y = -1.0f;
            obj->velocity.y = -obj->velocity.y * 0.5f;
        }
        
        obj->updateTransform();
    }
}

void UpdateAnimations(float deltaTime) {
    for (auto& obj : g_sceneObjects) {
        // Vérifications de base
        if (!obj->isAnimationPlaying || obj->currentAnimation == "None" || !obj->scene) {
            continue;
        }

        const aiScene* scene = obj->scene;
        aiAnimation* anim = nullptr;
        
        // Recherche de l'animation
        for (unsigned int i = 0; i < scene->mNumAnimations; i++) {
            std::string animName = scene->mAnimations[i]->mName.C_Str();
            if (animName.empty()) {
                animName = "Animation_" + std::to_string(i);
            }
            if (obj->currentAnimation == animName) {
                anim = scene->mAnimations[i];
                break;
            }
        }

        if (!anim) continue;

        // Mise à jour du temps
        float ticksPerSecond = (anim->mTicksPerSecond != 0) ? anim->mTicksPerSecond : 25.0f;
        obj->animationTime += deltaTime * ticksPerSecond;
        
        if (obj->animationTime > anim->mDuration) {
            obj->animationTime = fmod(obj->animationTime, anim->mDuration);
        }
        
        // ========================================
        // ANIMATIONS DE TRANSFORMATION
        // ========================================
        if (anim->mNumChannels == 0) continue;
        
        // Trouver le bon channel
        aiNodeAnim* nodeAnim = nullptr;
        if (scene->mRootNode) {
            std::string rootName = scene->mRootNode->mName.C_Str();
            for (unsigned int i = 0; i < anim->mNumChannels; i++) {
                if (std::string(anim->mChannels[i]->mNodeName.C_Str()) == rootName) {
                    nodeAnim = anim->mChannels[i];
                    break;
                }
            }
        }
        
        if (!nodeAnim && anim->mNumChannels > 0) {
            nodeAnim = anim->mChannels[0];
        }
        
        if (!nodeAnim) continue;
        
        // ===== INTERPOLATION POSITION =====
        if (nodeAnim->mNumPositionKeys > 0) {
            unsigned int frame = 0;
            for (unsigned int i = 0; i < nodeAnim->mNumPositionKeys - 1; i++) {
                if (obj->animationTime < (float)nodeAnim->mPositionKeys[i + 1].mTime) {
                    frame = i;
                    break;
                }
            }
            
            unsigned int nextFrame = (frame + 1) % nodeAnim->mNumPositionKeys;
            float t1 = (float)nodeAnim->mPositionKeys[frame].mTime;
            float t2 = (float)nodeAnim->mPositionKeys[nextFrame].mTime;
            float delta = (t2 - t1) == 0 ? 0 : (obj->animationTime - t1) / (t2 - t1);
            delta = glm::clamp(delta, 0.0f, 1.0f);
            
            const aiVector3D& v1 = nodeAnim->mPositionKeys[frame].mValue;
            const aiVector3D& v2 = nodeAnim->mPositionKeys[nextFrame].mValue;
            
            obj->position.x = v1.x + (v2.x - v1.x) * delta;
            obj->position.y = v1.y + (v2.y - v1.y) * delta;
            obj->position.z = v1.z + (v2.z - v1.z) * delta;
        }
        
        // ===== INTERPOLATION ROTATION =====
        if (nodeAnim->mNumRotationKeys > 0) {
            unsigned int frame = 0;
            for (unsigned int i = 0; i < nodeAnim->mNumRotationKeys - 1; i++) {
                if (obj->animationTime < (float)nodeAnim->mRotationKeys[i + 1].mTime) {
                    frame = i;
                    break;
                }
            }
            
            unsigned int nextFrame = (frame + 1) % nodeAnim->mNumRotationKeys;
            float t1 = (float)nodeAnim->mRotationKeys[frame].mTime;
            float t2 = (float)nodeAnim->mRotationKeys[nextFrame].mTime;
            float delta = (t2 - t1) == 0 ? 0 : (obj->animationTime - t1) / (t2 - t1);
            delta = glm::clamp(delta, 0.0f, 1.0f);
            
            const aiQuaternion& q1 = nodeAnim->mRotationKeys[frame].mValue;
            const aiQuaternion& q2 = nodeAnim->mRotationKeys[nextFrame].mValue;
            
            aiQuaternion result;
            aiQuaternion::Interpolate(result, q1, q2, delta);
            result.Normalize();
            
            glm::quat rotation(result.w, result.x, result.y, result.z);
            glm::vec3 euler = glm::eulerAngles(rotation);
            obj->rotation = glm::degrees(euler);
        }
        
        // ===== INTERPOLATION SCALE =====
        if (nodeAnim->mNumScalingKeys > 0) {
            unsigned int frame = 0;
            for (unsigned int i = 0; i < nodeAnim->mNumScalingKeys - 1; i++) {
                if (obj->animationTime < (float)nodeAnim->mScalingKeys[i + 1].mTime) {
                    frame = i;
                    break;
                }
            }
            
            unsigned int nextFrame = (frame + 1) % nodeAnim->mNumScalingKeys;
            float t1 = (float)nodeAnim->mScalingKeys[frame].mTime;
            float t2 = (float)nodeAnim->mScalingKeys[nextFrame].mTime;
            float delta = (t2 - t1) == 0 ? 0 : (obj->animationTime - t1) / (t2 - t1);
            delta = glm::clamp(delta, 0.0f, 1.0f);
            
            const aiVector3D& s1 = nodeAnim->mScalingKeys[frame].mValue;
            const aiVector3D& s2 = nodeAnim->mScalingKeys[nextFrame].mValue;
            
            obj->scale.x = s1.x + (s2.x - s1.x) * delta;
            obj->scale.y = s1.y + (s2.y - s1.y) * delta;
            obj->scale.z = s1.z + (s2.z - s1.z) * delta;
        }
        
        // ✅✅✅ CRUCIAL : Mettre à jour la matrice de transformation ✅✅✅
        obj->updateTransform();
    }
}

void SyncSceneCameraWithView() {
    if (g_sceneCamera == -1 || g_sceneCamera >= static_cast<int>(g_sceneObjects.size())) return;
    
    auto& camObj = g_sceneObjects[g_sceneCamera];
    if (camObj->type != ObjectType::CAMERA) return;

    g_camera.position = camObj->position;
    g_camera.yaw = camObj->rotation.y;
    g_camera.pitch = camObj->rotation.x;
    g_camera.fov = camObj->cameraFOV;
    g_camera.nearPlane = camObj->cameraNear;
    g_camera.farPlane = camObj->cameraFar;
    
    g_camera.updateVectors();
}

void RenderSkybox(const glm::mat4& view, const glm::mat4& projection) {
    glDepthFunc(GL_LEQUAL);
    glUseProgram(g_skyboxShader);
    
    GLint viewLoc = glGetUniformLocation(g_skyboxShader, "view");
    GLint projectionLoc = glGetUniformLocation(g_skyboxShader, "projection");
    GLint topColorLoc = glGetUniformLocation(g_skyboxShader, "topColor");
    GLint bottomColorLoc = glGetUniformLocation(g_skyboxShader, "bottomColor");

    glUniformMatrix4fv(viewLoc, 1, GL_FALSE, glm::value_ptr(view));
    glUniformMatrix4fv(projectionLoc, 1, GL_FALSE, glm::value_ptr(projection));
    glUniform3fv(topColorLoc, 1, glm::value_ptr(g_skyboxTopColor));
    glUniform3fv(bottomColorLoc, 1, glm::value_ptr(g_skyboxBottomColor));

    glBindVertexArray(g_skyboxVAO);
    glDrawArrays(GL_TRIANGLES, 0, 36);
    glBindVertexArray(0);
    glDepthFunc(GL_LESS);
}

void RenderScene(const glm::mat4& view, const glm::mat4& projection) {
    glUseProgram(g_shaderProgram);

    GLint viewLoc = glGetUniformLocation(g_shaderProgram, "view");
    GLint projectionLoc = glGetUniformLocation(g_shaderProgram, "projection");
    GLint modelLoc = glGetUniformLocation(g_shaderProgram, "model");
    GLint colorLoc = glGetUniformLocation(g_shaderProgram, "objectColor");
    GLint hasTextureLoc = glGetUniformLocation(g_shaderProgram, "hasTexture");
    GLint numLightsLoc = glGetUniformLocation(g_shaderProgram, "numLights");
    GLint viewPosLoc = glGetUniformLocation(g_shaderProgram, "viewPos");

    glUniformMatrix4fv(viewLoc, 1, GL_FALSE, glm::value_ptr(view));
    glUniformMatrix4fv(projectionLoc, 1, GL_FALSE, glm::value_ptr(projection));
    glUniform3fv(viewPosLoc, 1, glm::value_ptr(g_camera.position));

    std::vector<glm::vec3> lightPositions;
    std::vector<glm::vec3> lightColors;
    std::vector<float> lightIntensities;
    std::vector<float> lightRadii;

    for (const auto& obj : g_sceneObjects) {
        if (obj->type == ObjectType::LIGHT && obj->visible && lightPositions.size() < 8) {
            lightPositions.push_back(obj->position);
            lightColors.push_back(obj->lightColor);
            lightIntensities.push_back(obj->lightIntensity);
            lightRadii.push_back(obj->lightRadius);
        }
    }

    glUniform1i(numLightsLoc, static_cast<int>(lightPositions.size()));
    if (!lightPositions.empty()) {
        glUniform3fv(glGetUniformLocation(g_shaderProgram, "lightPositions"), lightPositions.size(), glm::value_ptr(lightPositions[0]));
        glUniform3fv(glGetUniformLocation(g_shaderProgram, "lightColors"), lightColors.size(), glm::value_ptr(lightColors[0]));
        glUniform1fv(glGetUniformLocation(g_shaderProgram, "lightIntensities"), lightIntensities.size(), lightIntensities.data());
        glUniform1fv(glGetUniformLocation(g_shaderProgram, "lightRadii"), lightRadii.size(), lightRadii.data());
    }

    for (const auto& obj : g_sceneObjects) {
        if (!obj->visible || obj->VAO == 0 || obj->indexCount == 0) continue;
        if (obj->type == ObjectType::CAMERA) continue;

        glUniformMatrix4fv(modelLoc, 1, GL_FALSE, glm::value_ptr(obj->transform));
        glUniform3fv(colorLoc, 1, glm::value_ptr(obj->color));

        bool hasTexture = !obj->textureName.empty() && g_textures.find(obj->textureName) != g_textures.end();
        glUniform1i(hasTextureLoc, hasTexture ? 1 : 0);

        if (hasTexture) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, g_textures[obj->textureName]);
            glUniform1i(glGetUniformLocation(g_shaderProgram, "texture1"), 0);
        }

        glBindVertexArray(obj->VAO);
        glDrawElements(GL_TRIANGLES, obj->indexCount, GL_UNSIGNED_INT, 0);
        glBindVertexArray(0);
    }
}

void MainLoop() {
    bool running = true;
    Uint32 lastTime = SDL_GetTicks();
    
    Log("Game started: " + g_gameConfig.gameName);
    
    while (running) {
        Uint32 currentTime = SDL_GetTicks();
        g_deltaTime = (currentTime - lastTime) / 1000.0f;
        g_deltaTime = glm::clamp(g_deltaTime, 0.0f, 0.1f);
        lastTime = currentTime;
        
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = false;
            }
            else if (event.type == SDL_KEYDOWN) {
                g_input.keys[event.key.keysym.scancode] = true;
                if (event.key.keysym.sym == SDLK_ESCAPE) {
                    running = false;
                }
            }
            else if (event.type == SDL_KEYUP) {
                g_input.keys[event.key.keysym.scancode] = false;
            }
            else if (event.type == SDL_MOUSEBUTTONDOWN) {
                g_input.mouseButtons[event.button.button] = true;
            }
            else if (event.type == SDL_MOUSEBUTTONUP) {
                g_input.mouseButtons[event.button.button] = false;
            }
            else if (event.type == SDL_MOUSEMOTION) {
                g_input.mouseX = event.motion.x;
                g_input.mouseY = event.motion.y;
            }
        }

        ProcessInput(g_deltaTime);
        UpdatePhysics(g_deltaTime);
        UpdateAnimations(g_deltaTime);
        
        for (auto& obj : g_sceneObjects) {
            obj->runScript();
        }
        
        SyncSceneCameraWithView();

        int width, height;
        SDL_GetWindowSize(g_window, &width, &height);
        glm::mat4 view = g_camera.getViewMatrix();
        glm::mat4 projection = g_camera.getProjectionMatrix((float)width / (float)height);

        glViewport(0, 0, width, height);
        glClearColor(0.1f, 0.1f, 0.15f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        RenderSkybox(view, projection);
        RenderScene(view, projection);

        SDL_GL_SwapWindow(g_window);
    }
}

int main(int argc, char** argv) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::cerr << "SDL Error: " << SDL_GetError() << std::endl;
        return 1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);

    LoadGameConfig();

    Uint32 windowFlags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE;
    if (g_gameConfig.fullscreen) {
        windowFlags |= SDL_WINDOW_FULLSCREEN;
    }

    g_window = SDL_CreateWindow(
        g_gameConfig.gameName.c_str(),
        SDL_WINDOWPOS_CENTERED, 
        SDL_WINDOWPOS_CENTERED,
        g_gameConfig.windowWidth, 
        g_gameConfig.windowHeight, 
        windowFlags
    );

    if (!g_window) {
        std::cerr << "Window creation error: " << SDL_GetError() << std::endl;
        SDL_Quit();
        return 1;
    }

    g_glContext = SDL_GL_CreateContext(g_window);
    if (!g_glContext) {
        std::cerr << "OpenGL context error: " << SDL_GetError() << std::endl;
        SDL_DestroyWindow(g_window);
        SDL_Quit();
        return 1;
    }

    if (!gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress)) {
        std::cerr << "GLAD initialization error" << std::endl;
        SDL_GL_DeleteContext(g_glContext);
        SDL_DestroyWindow(g_window);
        SDL_Quit();
        return 1;
    }

    SDL_GL_SetSwapInterval(1);

    glEnable(GL_DEPTH_TEST);
    glClearColor(0.1f, 0.1f, 0.15f, 1.0f);

    InitOpenGL();
    InitLua();
    LoadScene(g_gameConfig.sceneFile);

    if (g_sceneCamera == -1) {
        Log("WARNING: No camera found in scene! Using default camera.");
    } else {
        Log("Camera found at index: " + std::to_string(g_sceneCamera));
        SyncSceneCameraWithView();
    }

    MainLoop();

    glDeleteVertexArrays(1, &g_skyboxVAO);
    glDeleteBuffers(1, &g_skyboxVBO);
    glDeleteProgram(g_shaderProgram);
    glDeleteProgram(g_skyboxShader);
    
    for (auto& tex : g_textures) {
        glDeleteTextures(1, &tex.second);
    }
    
#ifndef NO_LUA
    if (g_luaState) {
        lua_close(g_luaState);
    }
#endif
    
    SDL_GL_DeleteContext(g_glContext);
    SDL_DestroyWindow(g_window);
    SDL_Quit();
    
    return 0;
}