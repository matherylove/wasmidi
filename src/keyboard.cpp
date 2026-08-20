#include "keyboard.hpp"
#include "mainwindow.hpp"

#include <GLES3/gl3.h>
#include <QOpenGLFramebufferObject>
#include <array>
#include <vector>

namespace {
struct KeyInstance { float x, y, w, h, black, active; };

GLuint shader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type); glShaderSource(s,1,&src,nullptr); glCompileShader(s);
    GLint ok=0; glGetShaderiv(s,GL_COMPILE_STATUS,&ok); if(!ok){ glDeleteShader(s); return 0; } return s;
}
GLuint program() {
    static const char* vs=R"(#version 300 es
precision highp float;
layout(location=0) in vec2 aCorner;
layout(location=1) in vec4 aRect;
layout(location=2) in vec2 aState;
out float vBlack; out float vActive;
void main(){ vec2 p=aRect.xy+aCorner*aRect.zw; gl_Position=vec4(p*2.0-1.0,0,1); vBlack=aState.x; vActive=aState.y; }
)";
    static const char* fs=R"(#version 300 es
precision mediump float;
in float vBlack; in float vActive; out vec4 fragColor;
void main(){ vec3 c=vBlack>0.5?vec3(.055,.047,.105):vec3(.78,.79,.86); if(vActive>0.5)c=mix(c,vec3(.65,.55,.98),.88); fragColor=vec4(c,1); }
)";
    GLuint v=shader(GL_VERTEX_SHADER,vs), f=shader(GL_FRAGMENT_SHADER,fs); if(!v||!f)return 0;
    GLuint p=glCreateProgram(); glAttachShader(p,v); glAttachShader(p,f); glLinkProgram(p); glDeleteShader(v); glDeleteShader(f); return p;
}

class Renderer final : public QQuickFramebufferObject::Renderer {
public:
    ~Renderer() override {
        if(vbo_) glDeleteBuffers(1,&vbo_); if(inst_) glDeleteBuffers(1,&inst_); if(vao_) glDeleteVertexArrays(1,&vao_); if(prog_) glDeleteProgram(prog_);
    }
    QOpenGLFramebufferObject* createFramebufferObject(const QSize& size) override { return new QOpenGLFramebufferObject(size); }
    void synchronize(QQuickFramebufferObject* item) override {
        auto* k=static_cast<Keyboard*>(item); auto* c=qobject_cast<MainWindow*>(k->controller());
        width_=int(k->width()); height_=int(k->height()); active_=c?c->activePitchMask():std::array<uint8_t,128>{}; dirty_=true;
    }
    void render() override {
        if(!prog_) init(); if(dirty_) rebuild();
        glViewport(0,0,width_,height_); glClearColor(.027f,.027f,.102f,1); glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(prog_); glBindVertexArray(vao_); glDrawArraysInstanced(GL_TRIANGLES,0,6,(GLsizei)keys_.size()); glBindVertexArray(0); update();
    }
private:
    void init(){
        prog_=program(); glGenVertexArrays(1,&vao_); glBindVertexArray(vao_);
        const float q[]={0,0,1,0,0,1,0,1,1,0,1,1}; glGenBuffers(1,&vbo_); glBindBuffer(GL_ARRAY_BUFFER,vbo_); glBufferData(GL_ARRAY_BUFFER,sizeof(q),q,GL_STATIC_DRAW); glEnableVertexAttribArray(0); glVertexAttribPointer(0,2,GL_FLOAT,0,0,nullptr);
        glGenBuffers(1,&inst_); glBindBuffer(GL_ARRAY_BUFFER,inst_); glEnableVertexAttribArray(1); glVertexAttribPointer(1,4,GL_FLOAT,0,sizeof(KeyInstance),(void*)0); glVertexAttribDivisor(1,1); glEnableVertexAttribArray(2); glVertexAttribPointer(2,2,GL_FLOAT,0,sizeof(KeyInstance),(void*)(4*sizeof(float))); glVertexAttribDivisor(2,1); glBindVertexArray(0);
    }
    void rebuild(){
        keys_.clear(); const int blackPattern[12]={0,1,0,1,0,0,1,0,1,0,1,0};
        int whiteTotal=0; for(int n=0;n<128;++n) if(!blackPattern[n%12]) ++whiteTotal;
        float ww=1.0f/float(whiteTotal), bw=ww*.62f; int wi=0;
        for(int n=0;n<128;++n) if(!blackPattern[n%12]) { keys_.push_back({wi*ww,0,ww*.985f,1,0,float(active_[n])}); ++wi; }
        wi=0; for(int n=0;n<128;++n){ bool b=blackPattern[n%12]; if(b){ float x=(wi*ww)-bw*.5f; keys_.push_back({x,.38f,bw,.62f,1,float(active_[n])}); } else ++wi; }
        glBindBuffer(GL_ARRAY_BUFFER,inst_); glBufferData(GL_ARRAY_BUFFER,keys_.size()*sizeof(KeyInstance),keys_.data(),GL_DYNAMIC_DRAW); dirty_=false;
    }
    GLuint prog_=0,vao_=0,vbo_=0,inst_=0; int width_=1,height_=1; bool dirty_=true; std::array<uint8_t,128> active_{}; std::vector<KeyInstance> keys_;
};
}

Keyboard::Keyboard(QQuickItem* parent):QQuickFramebufferObject(parent){}
void Keyboard::setController(QObject* c){ if(controller_==c)return; controller_=c; emit controllerChanged(); update(); }
QQuickFramebufferObject::Renderer* Keyboard::createRenderer() const { return new Renderer(); }
