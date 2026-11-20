#include <GL/gl.h>
#include <GL/glx.h>
#include <stdio.h>
int main(void) {
  Display *d=XOpenDisplay(NULL); if (!d) return 1;
  int attr[]={GLX_RGBA,GLX_DOUBLEBUFFER,None};
  XVisualInfo *v=glXChooseVisual(d, DefaultScreen(d), attr); if (!v) return 2;
  Window root=RootWindow(d,v->screen);
  XSetWindowAttributes a={0}; a.colormap=XCreateColormap(d,root,v->visual,AllocNone);
  Window w=XCreateWindow(d,root,0,0,64,64,0,v->depth,InputOutput,v->visual,CWColormap,&a);
  XMapWindow(d,w); XSync(d,False);
  GLXContext c=glXCreateContext(d,v,NULL,True);
  if (!c || !glXMakeCurrent(d,w,c)) return 3;
  glClearColor(0,1,0,1); glClear(GL_COLOR_BUFFER_BIT);
  printf("renderer=%s clear_err=%x\n",glGetString(GL_RENDERER),glGetError());
  const GLenum fmt[]={GL_RGBA,GL_RGB,GL_RGBA};
  const GLenum typ[]={GL_UNSIGNED_BYTE,GL_FLOAT,GL_FLOAT};
  for(int i=0;i<3;i++){
    unsigned char b[4]={42,42,42,42}; float f[4]={-7,-7,-7,-7};
    glReadPixels(5,5,1,1,fmt[i],typ[i],i?(void*)f:(void*)b);
    printf("fmt=%x type=%x err=%x byte=%u,%u,%u,%u float=%.1f,%.1f,%.1f,%.1f\n",fmt[i],typ[i],glGetError(),b[0],b[1],b[2],b[3],f[0],f[1],f[2],f[3]);
  }
  return 0;
}
