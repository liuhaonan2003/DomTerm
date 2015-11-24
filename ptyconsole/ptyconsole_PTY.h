/* DO NOT EDIT THIS FILE - it is machine generated */
#include <jni.h>
/* Header for class ptyconsole_PTY */

#ifndef _Included_ptyconsole_PTY
#define _Included_ptyconsole_PTY
#ifdef __cplusplus
extern "C" {
#endif
/*
 * Class:     ptyconsole_PTY
 * Method:    getTtyMode
 * Signature: (I)I
 */
JNIEXPORT jint JNICALL Java_ptyconsole_PTY_getTtyMode
  (JNIEnv *, jclass, jint);

/*
 * Class:     ptyconsole_PTY
 * Method:    init
 * Signature: ([[BLjava/lang/String;Ljava/lang/String;)I
 */
JNIEXPORT jint JNICALL Java_ptyconsole_PTY_init
  (JNIEnv *, jclass, jobjectArray, jstring, jstring);

/*
 * Class:     ptyconsole_PTY
 * Method:    writeToChildInput
 * Signature: (I[BII)V
 */
JNIEXPORT void JNICALL Java_ptyconsole_PTY_writeToChildInput__I_3BII
  (JNIEnv *, jclass, jint, jbyteArray, jint, jint);

/*
 * Class:     ptyconsole_PTY
 * Method:    writeToChildInput
 * Signature: (II)V
 */
JNIEXPORT void JNICALL Java_ptyconsole_PTY_writeToChildInput__II
  (JNIEnv *, jclass, jint, jint);

/*
 * Class:     ptyconsole_PTY
 * Method:    readFromChildOutput
 * Signature: (I[BII)I
 */
JNIEXPORT jint JNICALL Java_ptyconsole_PTY_readFromChildOutput__I_3BII
  (JNIEnv *, jclass, jint, jbyteArray, jint, jint);

/*
 * Class:     ptyconsole_PTY
 * Method:    readFromChildOutput
 * Signature: (I)I
 */
JNIEXPORT jint JNICALL Java_ptyconsole_PTY_readFromChildOutput__I
  (JNIEnv *, jclass, jint);

/*
 * Class:     ptyconsole_PTY
 * Method:    setWindowSize
 * Signature: (IIIII)V
 */
JNIEXPORT void JNICALL Java_ptyconsole_PTY_setWindowSize
  (JNIEnv *, jclass, jint, jint, jint, jint, jint);

#ifdef __cplusplus
}
#endif
#endif
