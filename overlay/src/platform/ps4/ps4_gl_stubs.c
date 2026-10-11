// Link-time stubs for SDL2 GLES/EGL symbols on PS4.
//
// Linking SDL2 statically pulls its GLES2 renderer + EGL video backend, whose
// only external Sony symbols are scePigletSetConfigurationVSH plus 75 gl*/egl* symbols.
// Because the PS4 port presents frames directly via native sceVideoOut, SDL video
// is never initialised (so none of these functions is ever called). Defining proper
// stubs here allows linking without -lScePigletv2VSH, removing all proprietary Sony
// module dependencies from the eboot.
//
// Plain C so the symbols have C linkage (matching SDL's references).

int scePigletSetConfigurationVSH(const void* cfg) { (void)cfg; return 0; }

int eglBindAPI(void) { return 0; }
int eglChooseConfig(void) { return 0; }
void* eglCreateContext(void) { return 0; }
void* eglCreatePbufferSurface(void) { return 0; }
void* eglCreateWindowSurface(void) { return 0; }
int eglDestroyContext(void) { return 0; }
int eglDestroySurface(void) { return 0; }
int eglGetConfigAttrib(void) { return 0; }
void* eglGetDisplay(void) { return 0; }
int eglGetError(void) { return 0; }
void* eglGetProcAddress(void) { return 0; }
int eglInitialize(void) { return 0; }
int eglMakeCurrent(void) { return 0; }
int eglQueryAPI(void) { return 0; }
const char* eglQueryString(void) { return 0; }
int eglSwapBuffers(void) { return 0; }
int eglSwapInterval(void) { return 0; }
int eglTerminate(void) { return 0; }
int eglWaitGL(void) { return 0; }
int eglWaitNative(void) { return 0; }

void glActiveTexture(void) {}
void glAttachShader(void) {}
void glBindAttribLocation(void) {}
void glBindBuffer(void) {}
void glBindFramebuffer(void) {}
void glBindTexture(void) {}
void glBlendEquationSeparate(void) {}
void glBlendFuncSeparate(void) {}
void glBufferData(void) {}
void glBufferSubData(void) {}
unsigned int glCheckFramebufferStatus(void) { return 0; }
void glClear(void) {}
void glClearColor(void) {}
void glCompileShader(void) {}
unsigned int glCreateProgram(void) { return 0; }
unsigned int glCreateShader(void) { return 0; }
void glDeleteBuffers(void) {}
void glDeleteFramebuffers(void) {}
void glDeleteProgram(void) {}
void glDeleteShader(void) {}
void glDeleteTextures(void) {}
void glDisable(void) {}
void glDisableVertexAttribArray(void) {}
void glDrawArrays(void) {}
void glEnable(void) {}
void glEnableVertexAttribArray(void) {}
void glFinish(void) {}
void glFramebufferTexture2D(void) {}
void glGenBuffers(void) {}
void glGenFramebuffers(void) {}
void glGenTextures(void) {}
int glGetAttribLocation(void) { return -1; }
unsigned int glGetError(void) { return 0; }
void glGetIntegerv(void) {}
void glGetProgramInfoLog(void) {}
void glGetProgramiv(void) {}
void glGetShaderInfoLog(void) {}
void glGetShaderiv(void) {}
const unsigned char* glGetString(void) { return 0; }
int glGetUniformLocation(void) { return -1; }
void glLinkProgram(void) {}
void glPixelStorei(void) {}
void glReadPixels(void) {}
void glScissor(void) {}
void glShaderBinary(void) {}
void glShaderSource(void) {}
void glTexImage2D(void) {}
void glTexParameteri(void) {}
void glTexSubImage2D(void) {}
void glUniform1i(void) {}
void glUniform4f(void) {}
void glUniformMatrix4fv(void) {}
void glUseProgram(void) {}
void glVertexAttribPointer(void) {}
void glViewport(void) {}
