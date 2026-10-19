/* Test program for glerr.dylib: raises one GL error (glEnd without glBegin) and makes one valid call.
 * Exit 77 when no CGL context can be created. */
#define GL_SILENCE_DEPRECATION
#include <OpenGL/CGLTypes.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <stdio.h>

int main(void)
{
    CGLPixelFormatAttribute attrs[] = {kCGLPFAColorSize, (CGLPixelFormatAttribute)24, (CGLPixelFormatAttribute)0};
    CGLPixelFormatObj pf = NULL;
    GLint npf = 0;
    if (CGLChoosePixelFormat(attrs, &pf, &npf) != kCGLNoError || pf == NULL)
        return 77;
    CGLContextObj ctx = NULL;
    if (CGLCreateContext(pf, NULL, &ctx) != kCGLNoError || ctx == NULL)
        return 77;
    CGLDestroyPixelFormat(pf);
    if (CGLSetCurrentContext(ctx) != kCGLNoError)
        return 77;
    glEnd();                          /* GL_INVALID_OPERATION: no glBegin */
    glBindTexture(GL_TEXTURE_2D, 0);  /* valid */
    return 0;
}
