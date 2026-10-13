/* Off-screen Adreno regression probe. No Android services or persistent writes.
 * Compare this identical executable with baseline and patched Mesa libraries.
 * A pass covers these GL transfers only; it does not establish a UI fix. */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 32
#define H 32
struct format { const char *name; GLenum internal; int bits[4]; };
static const struct format formats[] = {
    {"RGBA8",GL_RGBA8,{8,8,8,8}}, {"R8",GL_R8,{8,0,0,0}},
    {"RG8",GL_RG8,{8,8,0,0}}, {"RGB565",GL_RGB565,{5,6,5,0}},
    {"RGBA4",GL_RGBA4,{4,4,4,4}}, {"RGB5_A1",GL_RGB5_A1,{5,5,5,1}},
    {"RGB10_A2",GL_RGB10_A2,{10,10,10,2}},
    {"RGBA16F",GL_RGBA16F,{-1,-1,-1,-1}},
    {"R16F",GL_R16F,{-1,0,0,0}}, {"RG16F",GL_RG16F,{-1,-1,0,0}},
    {"RGBA32F",GL_RGBA32F,{-1,-1,-1,-1}},
    {"R32F",GL_R32F,{-1,0,0,0}}, {"RG32F",GL_RG32F,{-1,-1,0,0}},
};
static const GLfloat colors[4][4] = {
    {.19f,.41f,.73f,.63f}, {.81f,.27f,.11f,.37f},
    {.31f,.87f,.59f,.91f}, {.67f,.13f,.43f,.23f}
};
static void fatal(const char *why) {
    fprintf(stderr,"PROBE_FATAL %s EGL=%x GL=%x\n",why,eglGetError(),glGetError());
    exit(2);
}
static GLuint shader(GLenum type,const char *src) {
    GLuint s=glCreateShader(type); glShaderSource(s,1,&src,NULL); glCompileShader(s);
    GLint ok; glGetShaderiv(s,GL_COMPILE_STATUS,&ok);
    if(!ok) { char log[2048];glGetShaderInfoLog(s,sizeof(log),NULL,log);fprintf(stderr,"%s\n",log);fatal("shader"); }
    return s;
}
static GLuint program(void) {
    const char *vs="#version 300 es\nvoid main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position=vec4(p*2.-1.,0.,1.);}";
    const char *fs="#version 300 es\nprecision highp float;uniform sampler2D tex;out vec4 c;void main(){c=texelFetch(tex,ivec2(gl_FragCoord.xy),0);}";
    GLuint p=glCreateProgram(),v=shader(GL_VERTEX_SHADER,vs),f=shader(GL_FRAGMENT_SHADER,fs);
    glAttachShader(p,v);glAttachShader(p,f);glLinkProgram(p);
    GLint ok;glGetProgramiv(p,GL_LINK_STATUS,&ok);if(!ok)fatal("link");
    glDeleteShader(v);glDeleteShader(f);return p;
}
static GLuint texture(GLenum fmt) {
    GLuint t;glGenTextures(1,&t);glBindTexture(GL_TEXTURE_2D,t);
    glTexStorage2D(GL_TEXTURE_2D,1,fmt,W,H);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    return t;
}
static int attach(GLuint fb,GLuint t) {
    glBindFramebuffer(GL_FRAMEBUFFER,fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,t,0);
    return glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE;
}
static double quant(double x,int bits,int channel) {
    if(bits==0)return channel==3?1.:0.;
    if(bits<0)return x;
    double n=(1u<<bits)-1;return floor(x*n+.5)/n;
}
static void sample(GLuint t,GLuint outfb,GLuint outtex,unsigned char *pixels) {
    if(!attach(outfb,outtex))fatal("output framebuffer");
    glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,t);
    glDisable(GL_SCISSOR_TEST);glViewport(0,0,W,H);glDrawArrays(GL_TRIANGLES,0,3);
    glReadPixels(0,0,W,H,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
    GLenum e=glGetError();if(e){fprintf(stderr,"sample error %x\n",e);fatal("sample");}
}
static int compare(const unsigned char *p,const unsigned char *reference,const struct format *src,const struct format *dst,int *worst) {
    int bad=0;*worst=0;
    for(int y=0;y<H;y++)for(int x=0;x<W;x++) {
        int q=(x>=W/2)+2*(y>=H/2);
        for(int c=0;c<4;c++) {
            double value=reference?reference[4*(y*W+x)+c]/255.:quant(colors[q][c],src->bits[c],c);
            int want=(int)floor(255*quant(value,dst->bits[c],c)+.5);
            int d=abs((int)p[4*(y*W+x)+c]-want);
            if(d>*worst)*worst=d;
            /* Packed UNORM conversion may round either way by one destination
             * unit. Read the actual source, and allow that unit plus readback
             * quantization. Width/type errors are much larger. */
            int tolerance=2;
            if(dst->bits[c]>0 && dst->bits[c]<8)
                tolerance+=(int)ceil(255./((1u<<dst->bits[c])-1));
            if(d>tolerance)bad++;
        }
    }
    return bad;
}
int main(void) {
    setvbuf(stdout,NULL,_IONBF,0);
    PFNEGLGETPLATFORMDISPLAYEXTPROC getdisplay=(void*)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if(!getdisplay)fatal("platform display unavailable");
    EGLDisplay d=getdisplay(EGL_PLATFORM_SURFACELESS_MESA,EGL_DEFAULT_DISPLAY,NULL);
    EGLint major,minor;if(!eglInitialize(d,&major,&minor))fatal("eglInitialize");
    printf("EGL %d.%d vendor=%s\n",major,minor,eglQueryString(d,EGL_VENDOR));
    const EGLint ca[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT,EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE};
    EGLConfig config;EGLint count;if(!eglChooseConfig(d,ca,&config,1,&count)||!count)fatal("config");
    const EGLint ctxa[]={EGL_CONTEXT_CLIENT_VERSION,3,EGL_NONE};
    EGLContext ctx=eglCreateContext(d,config,EGL_NO_CONTEXT,ctxa);
    const EGLint sa[]={EGL_WIDTH,W,EGL_HEIGHT,H,EGL_NONE};
    EGLSurface surface=eglCreatePbufferSurface(d,config,sa);
    if(ctx==EGL_NO_CONTEXT||surface==EGL_NO_SURFACE||!eglMakeCurrent(d,surface,surface,ctx))fatal("context");
    const char *renderer=(const char*)glGetString(GL_RENDERER);
    printf("GL renderer=%s version=%s\n",renderer,glGetString(GL_VERSION));
    if(!renderer||!strstr(renderer,"FD512"))fatal("hardware FD512 required");
    glDisable(GL_DITHER);glDisable(GL_BLEND);glUseProgram(program());
    GLuint vao;glGenVertexArrays(1,&vao);glBindVertexArray(vao);
    GLuint fb[3];glGenFramebuffers(3,fb);GLuint out=texture(GL_RGBA8);
    unsigned char pixels[W*H*4],reference[W*H*4];int cases=0,failed=0,skips=0;
    for(unsigned s=0;s<sizeof(formats)/sizeof(formats[0]);s++) {
        GLuint src=texture(formats[s].internal);
        if(!attach(fb[0],src)) {printf("SKIP_SOURCE %s unsupported framebuffer\n",formats[s].name);glDeleteTextures(1,&src);skips++;while(glGetError()){}continue;}
        glEnable(GL_SCISSOR_TEST);
        for(int q=0;q<4;q++){glScissor((q&1)*W/2,(q>>1)*H/2,W/2,H/2);glClearBufferfv(GL_COLOR,0,colors[q]);}
        glDisable(GL_SCISSOR_TEST);
        sample(src,fb[2],out,reference);int worst,bad=compare(reference,NULL,&formats[s],&formats[s],&worst);
        printf("SOURCE_CONTROL %s bad=%d max_delta=%d\n",formats[s].name,bad,worst);
        if(bad)fatal("source reference failed");
        for(unsigned t=0;t<sizeof(formats)/sizeof(formats[0]);t++) {
            GLuint dst=texture(formats[t].internal);
            if(!attach(fb[1],dst)){skips++;glDeleteTextures(1,&dst);while(glGetError()){}continue;}
            glBindFramebuffer(GL_READ_FRAMEBUFFER,fb[0]);
            glFramebufferTexture2D(GL_READ_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,src,0);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER,fb[1]);
            glBlitFramebuffer(0,0,W,H,0,0,W,H,GL_COLOR_BUFFER_BIT,GL_NEAREST);
            GLenum error=glGetError();
            if(error){printf("SKIP_BLIT %s %s GL=%x\n",formats[s].name,formats[t].name,error);skips++;glDeleteTextures(1,&dst);continue;}
            sample(dst,fb[2],out,pixels);
            bad=compare(pixels,reference,&formats[s],&formats[t],&worst);cases++;if(bad)failed++;
            printf("CASE_BLIT %s %s bad=%d max_delta=%d\n",formats[s].name,formats[t].name,bad,worst);
            /* CopyTex does not have BlitFramebuffer's conditional-rendering
             * semantics and can take a different Gallium/2D transfer path. */
            attach(fb[1],dst);
            const GLfloat blank[4]={0,0,0,0};glClearBufferfv(GL_COLOR,0,blank);
            if(getenv("A6L_PROBE_FINISH_BEFORE_COPY"))glFinish();
            glBindFramebuffer(GL_READ_FRAMEBUFFER,fb[0]);
            glBindTexture(GL_TEXTURE_2D,dst);
            glCopyTexSubImage2D(GL_TEXTURE_2D,0,0,0,0,0,W,H);
            if(getenv("A6L_PROBE_FINISH_AFTER_COPY"))glFinish();
            error=glGetError();
            if(error){printf("SKIP_COPYTEX %s %s GL=%x\n",formats[s].name,formats[t].name,error);skips++;}
            else {
                sample(dst,fb[2],out,pixels);
                bad=compare(pixels,reference,&formats[s],&formats[t],&worst);cases++;if(bad)failed++;
                printf("CASE_COPYTEX %s %s bad=%d max_delta=%d\n",formats[s].name,formats[t].name,bad,worst);
            }
            glDeleteTextures(1,&dst);
        }
        glDeleteTextures(1,&src);
    }
    glFinish();printf("RESULT cases=%d failed=%d skips=%d\n",cases,failed,skips);
    eglMakeCurrent(d,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
    eglDestroySurface(d,surface);eglDestroyContext(d,ctx);eglTerminate(d);
    return cases==0?2:failed?1:0;
}
