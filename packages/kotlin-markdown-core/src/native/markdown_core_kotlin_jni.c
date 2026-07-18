#include "markdown_core_kotlin_bridge.h"

#include <jni.h>

static void throw_out_of_memory(JNIEnv *environment, const char *message) {
    jclass error_class = (*environment)->FindClass(environment, "java/lang/OutOfMemoryError");
    if (error_class != NULL) {
        (*environment)->ThrowNew(environment, error_class, message);
    }
}

static jbyteArray payload_to_array(JNIEnv *environment, uint8_t *output, size_t output_length) {
    jbyteArray result;
    if (output_length > (size_t)INT32_MAX) {
        markdown_core_kotlin_free(output);
        throw_out_of_memory(environment, "native payload exceeds the JVM array limit");
        return NULL;
    }
    result = (*environment)->NewByteArray(environment, (jsize)output_length);
    if (result != NULL) {
        (*environment)
            ->SetByteArrayRegion(environment, result, 0, (jsize)output_length,
                                 (const jbyte *)output);
    }
    markdown_core_kotlin_free(output);
    return result;
}

static markdown_core_kotlin_session *session_of(jlong handle) {
    return (markdown_core_kotlin_session *)(intptr_t)handle;
}

JNIEXPORT jbyteArray JNICALL Java_com_nouprax_markdown_core_JvmNative_parse(JNIEnv *environment,
                                                                            jobject receiver,
                                                                            jbyteArray source,
                                                                            jint options_mask) {
    jbyte *source_bytes;
    jsize source_length;
    uint8_t *output = NULL;
    size_t output_length = 0;
    (void)receiver;

    source_length = (*environment)->GetArrayLength(environment, source);
    source_bytes = (*environment)->GetByteArrayElements(environment, source, NULL);
    if (source_bytes == NULL) {
        return NULL;
    }
    if (!markdown_core_kotlin_parse((const uint8_t *)source_bytes, (size_t)source_length,
                                    (uint32_t)options_mask, &output, &output_length)) {
        (*environment)->ReleaseByteArrayElements(environment, source, source_bytes, JNI_ABORT);
        throw_out_of_memory(environment, "native AST copy failed");
        return NULL;
    }
    (*environment)->ReleaseByteArrayElements(environment, source, source_bytes, JNI_ABORT);
    return payload_to_array(environment, output, output_length);
}

JNIEXPORT jlong JNICALL Java_com_nouprax_markdown_core_JvmNative_sessionOpen(JNIEnv *environment,
                                                                             jobject receiver,
                                                                             jint options_mask) {
    (void)environment;
    (void)receiver;
    return (jlong)(intptr_t)markdown_core_kotlin_session_open((uint32_t)options_mask);
}

JNIEXPORT void JNICALL Java_com_nouprax_markdown_core_JvmNative_sessionFree(JNIEnv *environment,
                                                                            jobject receiver,
                                                                            jlong handle) {
    (void)environment;
    (void)receiver;
    markdown_core_kotlin_session_free(session_of(handle));
}

JNIEXPORT jlong JNICALL Java_com_nouprax_markdown_core_JvmNative_sessionLineage(JNIEnv *environment,
                                                                                jobject receiver,
                                                                                jlong handle) {
    (void)environment;
    (void)receiver;
    return (jlong)markdown_core_kotlin_session_lineage(session_of(handle));
}

JNIEXPORT jlong JNICALL Java_com_nouprax_markdown_core_JvmNative_sessionRevision(JNIEnv *environment,
                                                                                 jobject receiver,
                                                                                 jlong handle) {
    (void)environment;
    (void)receiver;
    return (jlong)markdown_core_kotlin_session_revision(session_of(handle));
}

JNIEXPORT jlong JNICALL Java_com_nouprax_markdown_core_JvmNative_sessionLength(JNIEnv *environment,
                                                                               jobject receiver,
                                                                               jlong handle) {
    (void)environment;
    (void)receiver;
    return (jlong)markdown_core_kotlin_session_length(session_of(handle));
}

JNIEXPORT jlong JNICALL Java_com_nouprax_markdown_core_JvmNative_sessionRoot(JNIEnv *environment,
                                                                             jobject receiver,
                                                                             jlong handle) {
    (void)environment;
    (void)receiver;
    return (jlong)markdown_core_kotlin_session_root(session_of(handle));
}

JNIEXPORT jbyteArray JNICALL Java_com_nouprax_markdown_core_JvmNative_sessionEdit(
    JNIEnv *environment, jobject receiver, jlong handle, jlong byte_start, jlong byte_end,
    jbyteArray replacement) {
    jbyte *replacement_bytes;
    jsize replacement_length;
    uint8_t *output = NULL;
    size_t output_length = 0;
    bool applied;
    (void)receiver;

    replacement_length = (*environment)->GetArrayLength(environment, replacement);
    replacement_bytes = (*environment)->GetByteArrayElements(environment, replacement, NULL);
    if (replacement_bytes == NULL) {
        return NULL;
    }
    applied = markdown_core_kotlin_session_edit(session_of(handle), (uint64_t)byte_start,
                                                (uint64_t)byte_end,
                                                (const uint8_t *)replacement_bytes,
                                                (size_t)replacement_length, &output, &output_length);
    (*environment)->ReleaseByteArrayElements(environment, replacement, replacement_bytes, JNI_ABORT);
    if (!applied) {
        throw_out_of_memory(environment, "native session edit failed");
        return NULL;
    }
    return payload_to_array(environment, output, output_length);
}

JNIEXPORT jbyteArray JNICALL Java_com_nouprax_markdown_core_JvmNative_sessionCommit(JNIEnv *environment,
                                                                                    jobject receiver,
                                                                                    jlong handle) {
    uint8_t *output = NULL;
    size_t output_length = 0;
    (void)receiver;

    if (!markdown_core_kotlin_session_commit(session_of(handle), &output, &output_length)) {
        throw_out_of_memory(environment, "native session commit failed");
        return NULL;
    }
    return payload_to_array(environment, output, output_length);
}

JNIEXPORT jbyteArray JNICALL Java_com_nouprax_markdown_core_JvmNative_sessionScopes(JNIEnv *environment,
                                                                                    jobject receiver,
                                                                                    jlong handle) {
    uint8_t *output = NULL;
    size_t output_length = 0;
    (void)receiver;

    if (!markdown_core_kotlin_session_scopes(session_of(handle), &output, &output_length)) {
        throw_out_of_memory(environment, "native scope table copy failed");
        return NULL;
    }
    return payload_to_array(environment, output, output_length);
}

JNIEXPORT jbyteArray JNICALL Java_com_nouprax_markdown_core_JvmNative_sessionFootnoteInfo(
    JNIEnv *environment, jobject receiver, jlong handle, jlong id) {
    uint8_t *output = NULL;
    size_t output_length = 0;
    (void)receiver;

    if (!markdown_core_kotlin_session_footnote_info(session_of(handle), (uint64_t)id, &output,
                                                    &output_length)) {
        throw_out_of_memory(environment, "native footnote info copy failed");
        return NULL;
    }
    return payload_to_array(environment, output, output_length);
}

JNIEXPORT jbyteArray JNICALL Java_com_nouprax_markdown_core_JvmNative_sessionFootnotes(
    JNIEnv *environment, jobject receiver, jlong handle) {
    uint8_t *output = NULL;
    size_t output_length = 0;
    (void)receiver;

    if (!markdown_core_kotlin_session_footnotes(session_of(handle), &output, &output_length)) {
        throw_out_of_memory(environment, "native footnote list copy failed");
        return NULL;
    }
    return payload_to_array(environment, output, output_length);
}

JNIEXPORT jbyteArray JNICALL Java_com_nouprax_markdown_core_JvmNative_sessionFootnoteReferences(
    JNIEnv *environment, jobject receiver, jlong handle, jlong definition) {
    uint8_t *output = NULL;
    size_t output_length = 0;
    (void)receiver;

    if (!markdown_core_kotlin_session_footnote_references(session_of(handle), (uint64_t)definition,
                                                          &output, &output_length)) {
        throw_out_of_memory(environment, "native footnote reference copy failed");
        return NULL;
    }
    return payload_to_array(environment, output, output_length);
}
