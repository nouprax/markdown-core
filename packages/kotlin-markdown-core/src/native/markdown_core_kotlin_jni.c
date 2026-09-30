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

static markdown_core_session *session_address(jlong session) { return (markdown_core_session *)(intptr_t)session; }

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

/* Makes a session of `source`, counted in UTF-16 when `utf16` is set and in
 * UTF-8 otherwise, and writes its address to `session[0]`. */
static jbyteArray JNICALL native_session_new(JNIEnv *environment, jobject receiver, jbyteArray source, jboolean utf16,
                                             jlongArray session) {
    jbyte *source_bytes;
    markdown_core_session *made;
    uint8_t *message;
    jbyteArray result;
    jlong address;
    (void)receiver;

    if (!pin_bytes(environment, source, &source_bytes)) {
        return NULL;
    }
    message = markdown_core_wire_session_new(
        (const uint8_t *)source_bytes, (size_t)(*environment)->GetArrayLength(environment, source),
        utf16 ? MARKDOWN_CORE_TEXT_UNIT_UTF16 : MARKDOWN_CORE_TEXT_UNIT_UTF8, &made);
    unpin_bytes(environment, source, source_bytes);
    result = message_array(environment, message);
    /* The session is Kotlin's only with its document's message. */
    if (result == NULL) {
        markdown_core_session_free(made);
        return NULL;
    }
    address = (jlong)(intptr_t)made;
    (*environment)->SetLongArrayRegion(environment, session, 0, 1, &address);
    return result;
}

/* Applies `fields.length / 3` edits: start, end and text size each, as
 * markdown_core_wire_session_edit takes them, with their texts in `texts`. */
static jbyteArray JNICALL native_session_edit(JNIEnv *environment, jobject receiver, jlong session, jintArray fields,
                                              jbyteArray texts) {
    jsize count;
    size_t *edits;
    jint *values;
    jbyte *text_bytes;
    uint8_t *message;
    (void)receiver;

    count = (*environment)->GetArrayLength(environment, fields);
    edits = NULL;
    if (count != 0) {
        edits = malloc((size_t)count * sizeof(*edits));
        if (edits == NULL) {
            return NULL;
        }
        values = (*environment)->GetIntArrayElements(environment, fields, NULL);
        if (values == NULL) {
            free(edits);
            return NULL;
        }
        for (jsize index = 0; index < count; ++index) {
            /* A negative offset converts to a size past the end of any text. */
            edits[index] = (size_t)values[index];
        }
        (*environment)->ReleaseIntArrayElements(environment, fields, values, JNI_ABORT);
    }
    if (!pin_bytes(environment, texts, &text_bytes)) {
        free(edits);
        return NULL;
    }
    message = markdown_core_wire_session_edit(session_address(session), edits, (size_t)count / 3,
                                              (const uint8_t *)text_bytes);
    unpin_bytes(environment, texts, text_bytes);
    free(edits);
    return message_array(environment, message);
}

static jbyteArray JNICALL native_session_append(JNIEnv *environment, jobject receiver, jlong session, jbyteArray text) {
    jbyte *text_bytes;
    uint8_t *message;
    (void)receiver;

    if (!pin_bytes(environment, text, &text_bytes)) {
        return NULL;
    }
    message = markdown_core_wire_session_append(session_address(session), (const uint8_t *)text_bytes,
                                                (size_t)(*environment)->GetArrayLength(environment, text));
    unpin_bytes(environment, text, text_bytes);
    return message_array(environment, message);
}

/* A copy of the session's text, which the 1 GiB limit keeps within a JVM
 * array. NULL leaves the JVM's exception pending. */
static jbyteArray JNICALL native_session_text(JNIEnv *environment, jobject receiver, jlong session) {
    const markdown_core_session *source;
    size_t size;
    jbyteArray result;
    void *bytes;
    (void)receiver;

    source = session_address(session);
    size = markdown_core_session_text_size(source);
    result = (*environment)->NewByteArray(environment, (jsize)size);
    if (result == NULL || size == 0) {
        return result;
    }
    bytes = (*environment)->GetPrimitiveArrayCritical(environment, result, NULL);
    if (bytes == NULL) {
        return NULL;
    }
    markdown_core_session_text(source, bytes);
    (*environment)->ReleasePrimitiveArrayCritical(environment, result, bytes, 0);
    return result;
}

static void JNICALL native_session_free(JNIEnv *environment, jobject receiver, jlong session) {
    (void)environment;
    (void)receiver;
    markdown_core_session_free(session_address(session));
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *virtual_machine, void *reserved) {
    static const JNINativeMethod methods[] = {
        {"parsePayload", "([B)[B", (void *)native_parse},
        {"sessionNewPayload", "([BZ[J)[B", (void *)native_session_new},
        {"sessionEditPayload", "(J[I[B)[B", (void *)native_session_edit},
        {"sessionAppendPayload", "(J[B)[B", (void *)native_session_append},
        {"sessionText", "(J)[B", (void *)native_session_text},
        {"sessionFree", "(J)V", (void *)native_session_free},
    };
    JNIEnv *environment;
    jclass parser_class;
    (void)reserved;

    (*virtual_machine)->GetEnv(virtual_machine, (void **)&environment, JNI_VERSION_1_6);
    parser_class = (*environment)->FindClass(environment, "com/nouprax/markdown/core/JniParser");
    (*environment)->RegisterNatives(environment, parser_class, methods, (jint)(sizeof(methods) / sizeof(methods[0])));
    return JNI_VERSION_1_6;
}
