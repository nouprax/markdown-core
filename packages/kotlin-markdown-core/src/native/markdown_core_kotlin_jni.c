#include "markdown_core_kotlin_bridge.h"

#include <jni.h>

/* One bridge payload becomes one Java byte array: the session's feed and
 * finish both funnel their MKC7 bytes through here. `succeeded == false` is
 * the bridge saying the payload buffer itself could not be built, which is
 * the one failure with no payload to decode -- it surfaces as the
 * OutOfMemoryError it is. */
static jbyteArray S_payload_to_array(JNIEnv *environment, bool succeeded, uint8_t *output, size_t output_length) {
    jbyteArray result;

    if (!succeeded) {
        jclass error_class = (*environment)->FindClass(environment, "java/lang/OutOfMemoryError");
        if (error_class != NULL) {
            (*environment)->ThrowNew(environment, error_class, "native AST copy failed");
        }
        return NULL;
    }
    if (output_length > (size_t)INT32_MAX) {
        markdown_core_kotlin_free(output);
        return NULL;
    }
    result = (*environment)->NewByteArray(environment, (jsize)output_length);
    if (result != NULL) {
        (*environment)->SetByteArrayRegion(environment, result, 0, (jsize)output_length, (const jbyte *)output);
    }
    markdown_core_kotlin_free(output);
    return result;
}

JNIEXPORT jlong JNICALL
Java_com_nouprax_markdown_core_JvmNative_sessionNew(JNIEnv *environment, jobject receiver, jint options_mask) {
    (void)environment;
    (void)receiver;
    return (jlong)(intptr_t)markdown_core_kotlin_session_new((uint32_t)options_mask);
}

JNIEXPORT jbyteArray JNICALL Java_com_nouprax_markdown_core_JvmNative_sessionFeed(
    JNIEnv *environment,
    jobject receiver,
    jlong session,
    jbyteArray chunk
) {
    jbyte *chunk_bytes = NULL;
    jsize chunk_length;
    uint8_t *output = NULL;
    size_t output_length = 0;
    bool succeeded;
    (void)receiver;

    /* An empty chunk is a legal feed and needs no elements: pinning a
     * zero-length array buys nothing and some JVMs answer it with NULL. */
    chunk_length = (*environment)->GetArrayLength(environment, chunk);
    if (chunk_length != 0) {
        /* Elements, DELIBERATELY not the critical view (#147): a critical
         * region must stay brief, and this one would span the whole parse,
         * whose duration is set by the input's shape -- an adversarial
         * chunk takes seconds. A collector that implements the critical
         * view by pinning holds up every thread's GC for as long as the
         * region is open: an application-wide stall traded for one input
         * copy. So the JVM binding pays the copy where the collector
         * charges one; JNI_ABORT releases the read-only view without
         * writing anything back. */
        chunk_bytes = (*environment)->GetByteArrayElements(environment, chunk, NULL);
        if (chunk_bytes == NULL) {
            return NULL;
        }
    }
    succeeded = markdown_core_kotlin_session_feed(
        (markdown_core_kotlin_session *)(intptr_t)session,
        (const uint8_t *)chunk_bytes,
        (size_t)chunk_length,
        &output,
        &output_length
    );
    if (chunk_bytes != NULL) {
        (*environment)->ReleaseByteArrayElements(environment, chunk, chunk_bytes, JNI_ABORT);
    }
    return S_payload_to_array(environment, succeeded, output, output_length);
}

JNIEXPORT jbyteArray JNICALL Java_com_nouprax_markdown_core_JvmNative_sessionAdvance(
    JNIEnv *environment,
    jobject receiver,
    jlong session,
    jbyteArray chunk
) {
    jbyte *chunk_bytes = NULL;
    jsize chunk_length;
    uint8_t *output = NULL;
    size_t output_length = 0;
    bool succeeded;
    (void)receiver;

    chunk_length = (*environment)->GetArrayLength(environment, chunk);
    if (chunk_length != 0) {
        /* The same read-only elements view the feed path takes, for the
         * same reason (#147). */
        chunk_bytes = (*environment)->GetByteArrayElements(environment, chunk, NULL);
        if (chunk_bytes == NULL) {
            return NULL;
        }
    }
    succeeded = markdown_core_kotlin_session_advance(
        (markdown_core_kotlin_session *)(intptr_t)session,
        (const uint8_t *)chunk_bytes,
        (size_t)chunk_length,
        &output,
        &output_length
    );
    if (chunk_bytes != NULL) {
        (*environment)->ReleaseByteArrayElements(environment, chunk, chunk_bytes, JNI_ABORT);
    }
    return S_payload_to_array(environment, succeeded, output, output_length);
}

JNIEXPORT jbyteArray JNICALL
Java_com_nouprax_markdown_core_JvmNative_sessionFinish(JNIEnv *environment, jobject receiver, jlong session) {
    uint8_t *output = NULL;
    size_t output_length = 0;
    bool succeeded;
    (void)receiver;

    succeeded =
        markdown_core_kotlin_session_finish((markdown_core_kotlin_session *)(intptr_t)session, &output, &output_length);
    return S_payload_to_array(environment, succeeded, output, output_length);
}

JNIEXPORT void JNICALL
Java_com_nouprax_markdown_core_JvmNative_sessionFree(JNIEnv *environment, jobject receiver, jlong session) {
    (void)environment;
    (void)receiver;
    markdown_core_kotlin_session_free((markdown_core_kotlin_session *)(intptr_t)session);
}
