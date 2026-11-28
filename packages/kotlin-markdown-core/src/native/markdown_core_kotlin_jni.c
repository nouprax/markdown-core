#include <markdown_core_wire.h>

#include <jni.h>
#include <stdint.h>
#include <stdlib.h>

/* The MCB3 message's u32 length, which counts the whole message. */
static uint32_t message_length(const uint8_t *message) {
    return (uint32_t)message[4] | (uint32_t)message[5] << 8 | (uint32_t)message[6] << 16 | (uint32_t)message[7] << 24;
}

/* Every entry point below returns an MCB3 message (docs/architecture/
 * wire-format.md) this way. NULL with no exception pending means
 * ALLOCATION_FAILED: the engine could not allocate the message, or it is longer
 * than a JVM array can hold. NULL with an exception pending is the JVM's own
 * failure to allocate. The Kotlin decoder owns everything after the copy, and
 * the engine's message is released here. */
static jbyteArray message_array(JNIEnv *environment, uint8_t *message) {
    uint32_t length;
    jbyteArray result;

    if (message == NULL) {
        return NULL;
    }
    length = message_length(message);
    result = NULL;
    if (length <= INT32_MAX) {
        /* NULL leaves the JVM's OutOfMemoryError pending. */
        result = (*environment)->NewByteArray(environment, (jsize)length);
        if (result != NULL) {
            (*environment)->SetByteArrayRegion(environment, result, 0, (jsize)length, (const jbyte *)message);
        }
    }
    markdown_core_wire_free(message);
    return result;
}

/* Points `*bytes` at the elements of `array`, NULL for an empty array. Zero
 * when the JVM cannot provide them, which leaves its exception pending. */
static int pin_bytes(JNIEnv *environment, jbyteArray array, jbyte **bytes) {
    *bytes = NULL;
    if ((*environment)->GetArrayLength(environment, array) == 0) {
        return 1;
    }
    *bytes = (*environment)->GetByteArrayElements(environment, array, NULL);
    return *bytes != NULL;
}

/* Releases what pin_bytes provided, without copying anything back. */
static void unpin_bytes(JNIEnv *environment, jbyteArray array, jbyte *bytes) {
    if (bytes != NULL) {
        (*environment)->ReleaseByteArrayElements(environment, array, bytes, JNI_ABORT);
    }
}

static jbyteArray JNICALL native_parse(JNIEnv *environment, jobject receiver, jbyteArray source) {
    jbyte *source_bytes;
    uint8_t *message;
    (void)receiver;

    if (!pin_bytes(environment, source, &source_bytes)) {
        return NULL;
    }
    message = markdown_core_wire_parse((const uint8_t *)source_bytes,
                                       (size_t)(*environment)->GetArrayLength(environment, source));
    unpin_bytes(environment, source, source_bytes);
    return message_array(environment, message);
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *virtual_machine, void *reserved) {
    static const JNINativeMethod methods[] = {
        {"parsePayload", "([B)[B", (void *)native_parse},
    };
    JNIEnv *environment;
    jclass parser_class;
    (void)reserved;

    (*virtual_machine)->GetEnv(virtual_machine, (void **)&environment, JNI_VERSION_1_6);
    parser_class = (*environment)->FindClass(environment, "com/nouprax/markdown/core/JniParser");
    (*environment)->RegisterNatives(environment, parser_class, methods, (jint)(sizeof(methods) / sizeof(methods[0])));
    return JNI_VERSION_1_6;
}
