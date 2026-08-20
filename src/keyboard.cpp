#include "keyboard.hpp"
#include "mainwindow.hpp"

#include <GLES3/gl3.h>
#include <QOpenGLFramebufferObject>
#include <QQuickOpenGLUtils>
#include <QQuickWindow>
#include <algorithm>
#include <array>
#include <vector>

namespace {

struct KeyInstance {
    float x,y,w,h;
    float r,g,b,active;
};

GLuint compileShader(GLenum type,const char* src){
    GLuint s=glCreateShader(type);
    glShaderSource(s,1,&src,nullptr);
    glCompileShader(s);
    GLint ok=0;glGetShaderiv(s,GL_COMPILE_STATUS,&ok);
    if(!ok){glDeleteShader(s);return 0;}return s;
}

GLuint makeProgram(){
    static const char* vs=R"GLSL(#version 300 es
precision highp float;
layout(location=0) in vec2 aCorner;
layout(location=1) in vec4 aRect;
layout(location=2) in vec4 aColor;
out vec2 vUv;
out vec4 vColor;
out float vBlack;
void main(){
    vec2 p=aRect.xy+aCorner*aRect.zw;
    gl_Position=vec4(p*2.0-1.0,0,1);
    vUv=aCorner;vColor=aColor;
    vBlack=float(aRect.w<0.8);
}
)GLSL";
    static const char* fs=R"GLSL(#version 300 es
precision mediump float;
in vec2 vUv;in vec4 vColor;in float vBlack;
uniform float uHue;
out vec4 fragColor;
vec3 hsv2rgb(vec3 c){vec4 K=vec4(1.,2./3.,1./3.,3.);vec3 p=abs(fract(c.xxx+K.xyz)*6.-K.www);return c.z*mix(K.xxx,clamp(p-K.xxx,0.,1.),c.y);}
void main(){
    float h=fract(uHue/360.0);
    vec3 top=vBlack>.5?hsv2rgb(vec3(h,.20,.12)):hsv2rgb(vec3(h,.30,.18));
    vec3 bot=vBlack>.5?hsv2rgb(vec3(h,.15,.03)):hsv2rgb(vec3(h,.22,.07));
    vec3 c=mix(top,bot,vUv.y);
    if(vColor.a>.5)c=mix(c,vColor.rgb,vBlack>.5?.94:.86);
    if(vBlack<.5&&min(vUv.x,1.-vUv.x)<.025)c*=.58;
    fragColor=vec4(c,1);
}
)GLSL";
    GLuint v=compileShader(GL_VERTEX_SHADER,vs),f=compileShader(GL_FRAGMENT_SHADER,fs);
    if(!v||!f)return 0;
    GLuint p=glCreateProgram();glAttachShader(p,v);glAttachShader(p,f);glLinkProgram(p);
    glDeleteShader(v);glDeleteShader(f);GLint ok=0;glGetProgramiv(p,GL_LINK_STATUS,&ok);
    if(!ok){glDeleteProgram(p);return 0;}return p;
}

class KeyboardRenderer final:public QQuickFramebufferObject::Renderer{
public:
    explicit KeyboardRenderer(qreal dpr):dpr_(std::max<qreal>(1.0,dpr)){}
    ~KeyboardRenderer() override{
        if(vbo_)glDeleteBuffers(1,&vbo_);
        if(inst_)glDeleteBuffers(1,&inst_);
        if(vao_)glDeleteVertexArrays(1,&vao_);
        if(prog_)glDeleteProgram(prog_);
    }
    QOpenGLFramebufferObject* createFramebufferObject(const QSize& s) override{
        QSize css(std::max(64,qRound(s.width()/dpr_)),std::max(64,qRound(s.height()/dpr_)));
        return new QOpenGLFramebufferObject(css);
    }
    void synchronize(QQuickFramebufferObject* item) override{
        auto*k=static_cast<Keyboard*>(item);
        auto*c=qobject_cast<MainWindow*>(k->controller());
        if(!c)return;
        mask_=c->activePitchMask();
        indices_=c->activePitchColorIndices();
        colors_=c->channelColors();
        hue_=c->dominantHue();
        dirty_=true;
    }
    void render() override{
        if(!prog_)init(); if(!prog_)return;
        if(auto*fbo=framebufferObject()){w_=std::max(1,fbo->size().width());h_=std::max(1,fbo->size().height());}
        if(dirty_)rebuild();
        glViewport(0,0,w_,h_);glDisable(GL_DEPTH_TEST);glDisable(GL_BLEND);
        glClearColor(.01,.01,.025,1);glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(prog_);glUniform1f(hueLoc_,hue_);
        glBindVertexArray(vao_);glDrawArraysInstanced(GL_TRIANGLES,0,6,GLsizei(keys_.size()));glBindVertexArray(0);
        QQuickOpenGLUtils::resetOpenGLState();
    }
private:
    void init(){
        prog_=makeProgram(); if(!prog_)return;
        hueLoc_=glGetUniformLocation(prog_,"uHue");
        const float q[]={0,0,1,0,0,1,0,1,1,0,1,1};
        glGenVertexArrays(1,&vao_);glBindVertexArray(vao_);
        glGenBuffers(1,&vbo_);glBindBuffer(GL_ARRAY_BUFFER,vbo_);glBufferData(GL_ARRAY_BUFFER,sizeof(q),q,GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,nullptr);
        glGenBuffers(1,&inst_);glBindBuffer(GL_ARRAY_BUFFER,inst_);
        glEnableVertexAttribArray(1);glVertexAttribPointer(1,4,GL_FLOAT,GL_FALSE,sizeof(KeyInstance),(void*)0);glVertexAttribDivisor(1,1);
        glEnableVertexAttribArray(2);glVertexAttribPointer(2,4,GL_FLOAT,GL_FALSE,sizeof(KeyInstance),(void*)(4*sizeof(float)));glVertexAttribDivisor(2,1);
        glBindVertexArray(0);
    }
    std::array<float,4> col(int note)const{
        if(!mask_[note]||indices_[note]<0||indices_[note]>=colors_.size())return{0,0,0,0};
        QColor c=colors_[indices_[note]];
        return{float(c.redF()),float(c.greenF()),float(c.blueF()),1};
    }
    void rebuild(){
        static const bool black[12]={0,1,0,1,0,0,1,0,1,0,1,0};
        keys_.clear();int wc=0;for(int n=0;n<128;n++)if(!black[n%12])wc++;
        float ww=1.f/float(wc),bw=ww*.58f;int wi=0;
        for(int n=0;n<128;n++){if(black[n%12])continue;auto c=col(n);keys_.push_back({wi*ww,0,ww,1,c[0],c[1],c[2],c[3]});wi++;}
        wi=0;for(int n=0;n<128;n++){if(black[n%12]){float x=wi*ww-bw*.5f;auto c=col(n);keys_.push_back({x,0,bw,.62f,c[0],c[1],c[2],c[3]});}else wi++;}
        glBindBuffer(GL_ARRAY_BUFFER,inst_);glBufferData(GL_ARRAY_BUFFER,GLsizeiptr(keys_.size()*sizeof(KeyInstance)),keys_.data(),GL_DYNAMIC_DRAW);dirty_=false;
    }
    qreal dpr_=1;GLuint prog_=0,vao_=0,vbo_=0,inst_=0;GLint hueLoc_=-1;int w_=1,h_=1;bool dirty_=true;float hue_=230;
    std::array<uint8_t,128> mask_{};std::array<int8_t,128> indices_{};QVector<QColor> colors_;std::vector<KeyInstance> keys_;
};
}
Keyboard::Keyboard(QQuickItem* p):QQuickFramebufferObject(p){setMirrorVertically(false);setTextureFollowsItemSize(true);}
void Keyboard::setController(QObject*c){
    if(controller_==c)return;if(controller_)QObject::disconnect(controller_.data(),nullptr,this,nullptr);controller_=c;
    if(auto*p=qobject_cast<MainWindow*>(controller_.data())){
        auto r=[this](){update();};
        connect(p,&MainWindow::currentTimeChanged,this,r);
        connect(p,&MainWindow::channelColorsChanged,this,r);
        connect(p,&MainWindow::neuralVisualChanged,this,r);
        connect(p,&MainWindow::documentRevisionChanged,this,r);
    }
    emit controllerChanged();update();
}
QQuickFramebufferObject::Renderer* Keyboard::createRenderer()const{
    qreal dpr=window()?window()->devicePixelRatio():1.0;return new KeyboardRenderer(dpr);
}
