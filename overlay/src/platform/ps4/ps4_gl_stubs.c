// Link-time stubs for SDL2 GLES/EGL symbols on PS4.
//
// Linking SDL2 statically pulls its GLES2 renderer + EGL video backend, whose
// only external Sony symbols are scePigletSetConfigurationVSH plus 75 gl*/egl* symbols.
// Because the PS4 port presents frames directly via native sceVideoOut, SDL video
// is never initialised (so none of these functions is ever called). Defining empty
// stubs here allows linking without -lScePigletv2VSH, removing all proprietary Sony
// module dependencies from the eboot.
//
// Plain C so the symbols have C linkage (matching SDL's references).

int scePigletSetConfigurationVSH(const void* cfg) { (void)cfg; return 0; }

void eglBindAPI(void) {}
void eglChooseConfig(void) {}
void eglCreateContext(void) {}
void eglCreatePbufferSurface(void) {}
void eglCreateWindowSurface(void) {}
void eglDestroyContext(void) {}
void eglDestroySurface(void) {}
void eglGetConfigAttrib(void) {}
void eglGetDisplay(void) {}
void eglGetError(void) {}
void eglGetProcAddress(void) {}
void eglInitialize(void) {}
void eglMakeCurrent(void) {}
void eglQueryAPI(void) {}
void eglQueryString(void) {}
void eglSwapBuffers(void) {}
void eglSwapInterval(void) {}
void eglTerminate(void) {}
void eglWaitGL(void) {}
void eglWaitNative(void) {}
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
void glCheckFramebufferStatus(void) {}
void glClear(void) {}
void glClearColor(void) {}
void glCompileShader(void) {}
void glCreateProgram(void) {}
void glCreateShader(void) {}
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
void glGetAttribLocation(void) {}
void glGetError(void) {}
void glGetIntegerv(void) {}
void glGetProgramInfoLog(void) {}
void glGetProgramiv(void) {}
void glGetShaderInfoLog(void) {}
void glGetShaderiv(void) {}
void glGetString(void) {}
void glGetUniformLocation(void) {}
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
