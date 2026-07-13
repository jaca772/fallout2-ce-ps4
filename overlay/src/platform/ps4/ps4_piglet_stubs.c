// Link-time stubs for the native sceVideoOut build (PS4_NATIVE_VIDEOOUT).
//
// Linking SDL2 statically pulls its GLES2 renderer + EGL/Piglet video backend, whose
// only EXTERNAL Sony symbol is scePigletSetConfigurationVSH, plus 75 gl*/egl* symbols
// normally provided by the Sony Piglet module. In the native build we never init SDL
// video (so none of these is ever CALLED), which lets us link WITHOUT
// -lScePigletv2VSH by defining them here as empty. This removes the Sony module
// dependency from the eboot.
//
// Plain C so the symbols have C linkage (matching SDL's references). Guarded so the
// DEFAULT build (which links the real -lScePigletv2VSH) compiles this to nothing.

#ifdef PS4_NATIVE_VIDEOOUT

// The one Sony-module function SDL's video backend references.
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

#endif // PS4_NATIVE_VIDEOOUT
