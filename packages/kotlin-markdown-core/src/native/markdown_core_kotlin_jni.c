#include <markdown_core_lsp.h>
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
        markdown_core_session_close(made);
        return NULL;
    }
    address = (jlong)(intptr_t)made;
    (*environment)->SetLongArrayRegion(environment, session, 0, 1, &address);
    return result;
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

static jbyteArray JNICALL native_session_flush(JNIEnv *environment, jobject receiver, jlong session) {
    (void)receiver;
    return message_array(environment, markdown_core_wire_session_flush(session_address(session)));
}

static jbyteArray JNICALL native_session_edit(JNIEnv *environment, jobject receiver, jlong session, jint start_line,
                                              jint start_column, jint end_line, jint end_column, jbyteArray text) {
    markdown_core_position from = {start_line, start_column}, to = {end_line, end_column};
    jbyte *pinned;
    uint8_t *message;
    (void)receiver;

    if (!pin_bytes(environment, text, &pinned)) {
        return NULL;
    }
    message = markdown_core_wire_session_edit(session_address(session), from, to, (const uint8_t *)pinned,
                                              (size_t)(*environment)->GetArrayLength(environment, text));
    unpin_bytes(environment, text, pinned);
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

static void JNICALL native_session_close(JNIEnv *environment, jobject receiver, jlong session) {
    (void)environment;
    (void)receiver;
    markdown_core_session_close(session_address(session));
}

static jbyteArray JNICALL native_dialect(JNIEnv *environment, jobject receiver) {
    (void)receiver;
    return message_array(environment, markdown_core_wire_dialect());
}

/* Locates the session at `line` and `column` and answers its context. */
static jbyteArray JNICALL native_session_locate(JNIEnv *environment, jobject receiver, jlong session, jint line,
                                                jint column) {
    markdown_core_position position = {line, column};
    (void)receiver;
    return message_array(environment, markdown_core_wire_locate(session_address(session), position));
}

static jbyteArray JNICALL native_session_spell(JNIEnv *environment, jobject receiver, jlong session, jint slot,
                                               jbyteArray value) {
    jbyte *value_bytes;
    uint8_t *message;
    (void)receiver;

    if (!pin_bytes(environment, value, &value_bytes)) {
        return NULL;
    }
    message = markdown_core_wire_spell(session_address(session), (size_t)slot, (const uint8_t *)value_bytes,
                                       (size_t)(*environment)->GetArrayLength(environment, value));
    unpin_bytes(environment, value, value_bytes);
    return message_array(environment, message);
}

static jbyteArray JNICALL native_session_field(JNIEnv *environment, jobject receiver, jlong session, jlong id,
                                               jint line, jint column, jint slot) {
    markdown_core_position position = {line, column};
    (void)receiver;
    return message_array(environment, markdown_core_wire_field(session_address(session), (uint64_t)id, position,
                                                               (markdown_core_slot_kind)slot));
}

static jbyteArray JNICALL native_session_markers(JNIEnv *environment, jobject receiver, jlong session, jlong id,
                                                 jint line, jint column) {
    markdown_core_position position = {line, column};
    (void)receiver;
    return message_array(environment, markdown_core_wire_markers(session_address(session), (uint64_t)id, position));
}

/* The language engine (packages/markdown-core/lsp): each entry point below is
 * one call of markdown_core_lsp_*, and answers its status. A JVM failure to
 * pin or allocate leaves its exception pending, and the status is then
 * MARKDOWN_CORE_LSP_INTERNAL_ERROR, which the pending exception overrides. */
static markdown_core_lsp_server *server_address(jlong server) { return (markdown_core_lsp_server *)(intptr_t)server; }

/* Writes a new server's address to `server[0]`. */
static jint JNICALL native_server_new(JNIEnv *environment, jobject receiver, jlongArray server) {
    markdown_core_lsp_server *made;
    markdown_core_lsp_status status;
    jlong address;
    (void)receiver;

    status = markdown_core_lsp_server_new(&made);
    if (status == MARKDOWN_CORE_LSP_OK) {
        address = (jlong)(intptr_t)made;
        (*environment)->SetLongArrayRegion(environment, server, 0, 1, &address);
    }
    return status;
}

static void JNICALL native_server_free(JNIEnv *environment, jobject receiver, jlong server) {
    (void)environment;
    (void)receiver;
    markdown_core_lsp_server_free(server_address(server));
}

static jint JNICALL native_server_phase(JNIEnv *environment, jobject receiver, jlong server) {
    (void)environment;
    (void)receiver;
    return markdown_core_lsp_server_phase(server_address(server));
}

/* Initializes with the units `utf16` names, true for UTF-16 and false for
 * UTF-8. `answer[0]` receives the status and, on success, `answer[1]` 1 when
 * the server counts in UTF-16 and 0 otherwise; the result is the cues. */
static jbyteArray JNICALL native_server_initialize(JNIEnv *environment, jobject receiver, jlong server,
                                                   jbooleanArray utf16, jintArray answer) {
    jsize count, index;
    jboolean *flags;
    markdown_core_text_unit *units;
    markdown_core_lsp_capabilities capabilities;
    jint written[2] = {MARKDOWN_CORE_LSP_INTERNAL_ERROR, 0};
    jbyteArray cues;
    (void)receiver;

    count = (*environment)->GetArrayLength(environment, utf16);
    units = count > 0 ? malloc((size_t)count * sizeof *units) : NULL;
    flags = count > 0 ? (*environment)->GetBooleanArrayElements(environment, utf16, NULL) : NULL;
    if (count > 0 && (units == NULL || flags == NULL)) {
        if (flags != NULL) {
            (*environment)->ReleaseBooleanArrayElements(environment, utf16, flags, JNI_ABORT);
        }
        free(units);
        (*environment)->SetIntArrayRegion(environment, answer, 0, 2, written);
        return NULL;
    }
    for (index = 0; index < count; index++) {
        units[index] = flags[index] ? MARKDOWN_CORE_TEXT_UNIT_UTF16 : MARKDOWN_CORE_TEXT_UNIT_UTF8;
    }
    if (flags != NULL) {
        (*environment)->ReleaseBooleanArrayElements(environment, utf16, flags, JNI_ABORT);
    }
    written[0] = markdown_core_lsp_initialize(server_address(server), units, (size_t)count, &capabilities);
    free(units);
    cues = NULL;
    if (written[0] == MARKDOWN_CORE_LSP_OK) {
        written[1] = capabilities.unit == MARKDOWN_CORE_TEXT_UNIT_UTF16;
        /* The cues are a few ASCII bytes. */
        cues = (*environment)->NewByteArray(environment, (jsize)capabilities.cues.length);
        if (cues != NULL) {
            (*environment)
                ->SetByteArrayRegion(environment, cues, 0, (jsize)capabilities.cues.length,
                                     (const jbyte *)capabilities.cues.data);
        }
    }
    (*environment)->SetIntArrayRegion(environment, answer, 0, 2, written);
    return cues;
}

static jint JNICALL native_server_shutdown(JNIEnv *environment, jobject receiver, jlong server) {
    (void)environment;
    (void)receiver;
    return markdown_core_lsp_shutdown(server_address(server));
}

static jint JNICALL native_server_open(JNIEnv *environment, jobject receiver, jlong server, jbyteArray uri,
                                       jint version, jbyteArray text) {
    jbyte *name, *bytes;
    jint status;
    (void)receiver;

    if (!pin_bytes(environment, uri, &name)) {
        return MARKDOWN_CORE_LSP_INTERNAL_ERROR;
    }
    if (!pin_bytes(environment, text, &bytes)) {
        unpin_bytes(environment, uri, name);
        return MARKDOWN_CORE_LSP_INTERNAL_ERROR;
    }
    status = markdown_core_lsp_open(server_address(server), (const uint8_t *)name,
                                    (size_t)(*environment)->GetArrayLength(environment, uri), version,
                                    (const uint8_t *)bytes, (size_t)(*environment)->GetArrayLength(environment, text));
    unpin_bytes(environment, text, bytes);
    unpin_bytes(environment, uri, name);
    return status;
}

/* Applies the edits whose scopes `scopes` holds, four unsigned numbers each,
 * and whose texts are `text` cut at `ends`. */
static jint JNICALL native_server_update(JNIEnv *environment, jobject receiver, jlong server, jbyteArray uri,
                                         jint version, jintArray scopes, jbyteArray text, jintArray ends) {
    jsize count, index;
    jbyte *name, *bytes;
    jint *bounds, *cuts;
    markdown_core_lsp_edit *edits;
    jint status, start;
    (void)receiver;

    count = (*environment)->GetArrayLength(environment, ends);
    status = MARKDOWN_CORE_LSP_INTERNAL_ERROR;
    name = bytes = NULL;
    bounds = cuts = NULL;
    edits = count > 0 ? malloc((size_t)count * sizeof *edits) : NULL;
    if ((count > 0 && edits == NULL) || !pin_bytes(environment, uri, &name)) {
        free(edits);
        return status;
    }
    if (!pin_bytes(environment, text, &bytes)) {
        goto done;
    }
    if (count > 0) {
        bounds = (*environment)->GetIntArrayElements(environment, scopes, NULL);
        cuts = (*environment)->GetIntArrayElements(environment, ends, NULL);
        if (bounds == NULL || cuts == NULL) {
            goto done;
        }
    }
    start = 0;
    for (index = 0; index < count; index++) {
        edits[index].scope.start.line = (uint32_t)bounds[index * 4];
        edits[index].scope.start.character = (uint32_t)bounds[index * 4 + 1];
        edits[index].scope.end.line = (uint32_t)bounds[index * 4 + 2];
        edits[index].scope.end.character = (uint32_t)bounds[index * 4 + 3];
        edits[index].text = bytes != NULL ? (const uint8_t *)bytes + start : NULL;
        edits[index].size = (size_t)(cuts[index] - start);
        start = cuts[index];
    }
    status = markdown_core_lsp_update(server_address(server), (const uint8_t *)name,
                                      (size_t)(*environment)->GetArrayLength(environment, uri), version, edits,
                                      (size_t)count);
done:
    if (cuts != NULL) {
        (*environment)->ReleaseIntArrayElements(environment, ends, cuts, JNI_ABORT);
    }
    if (bounds != NULL) {
        (*environment)->ReleaseIntArrayElements(environment, scopes, bounds, JNI_ABORT);
    }
    unpin_bytes(environment, text, bytes);
    unpin_bytes(environment, uri, name);
    free(edits);
    return status;
}

static jint JNICALL native_server_close(JNIEnv *environment, jobject receiver, jlong server, jbyteArray uri) {
    jbyte *name;
    jint status;
    (void)receiver;

    if (!pin_bytes(environment, uri, &name)) {
        return MARKDOWN_CORE_LSP_INTERNAL_ERROR;
    }
    status = markdown_core_lsp_close(server_address(server), (const uint8_t *)name,
                                     (size_t)(*environment)->GetArrayLength(environment, uri));
    unpin_bytes(environment, uri, name);
    return status;
}

/* Completes at `line` and `character`, read as unsigned; on success
 * `answer` receives the list's version and count. */
static jint JNICALL native_server_complete(JNIEnv *environment, jobject receiver, jlong server, jbyteArray uri,
                                           jint line, jint character, jlongArray answer) {
    markdown_core_lsp_position position;
    markdown_core_lsp_list *list;
    jbyte *name;
    jint status;
    jlong written[2];
    (void)receiver;

    if (!pin_bytes(environment, uri, &name)) {
        return MARKDOWN_CORE_LSP_INTERNAL_ERROR;
    }
    position.line = (uint32_t)line;
    position.character = (uint32_t)character;
    status = markdown_core_lsp_complete(server_address(server), (const uint8_t *)name,
                                        (size_t)(*environment)->GetArrayLength(environment, uri), position, &list);
    unpin_bytes(environment, uri, name);
    if (status == MARKDOWN_CORE_LSP_OK) {
        written[0] = markdown_core_lsp_list_version(list);
        written[1] = (jlong)markdown_core_lsp_list_count(list);
        markdown_core_lsp_list_free(list);
        (*environment)->SetLongArrayRegion(environment, answer, 0, 2, written);
    }
    return status;
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *virtual_machine, void *reserved) {
    static const JNINativeMethod methods[] = {
        {"parsePayload", "([B)[B", (void *)native_parse},
        {"sessionNewPayload", "([BZ[J)[B", (void *)native_session_new},
        {"sessionAppendPayload", "(J[B)[B", (void *)native_session_append},
        {"sessionFlushPayload", "(J)[B", (void *)native_session_flush},
        {"sessionEditPayload", "(JIIII[B)[B", (void *)native_session_edit},
        {"sessionText", "(J)[B", (void *)native_session_text},
        {"sessionClose", "(J)V", (void *)native_session_close},
        {"dialectAnswer", "()[B", (void *)native_dialect},
        {"sessionLocateAnswer", "(JII)[B", (void *)native_session_locate},
        {"sessionSpellAnswer", "(JI[B)[B", (void *)native_session_spell},
        {"sessionFieldAnswer", "(JJIII)[B", (void *)native_session_field},
        {"sessionMarkersAnswer", "(JJII)[B", (void *)native_session_markers},
        {"serverNew", "([J)I", (void *)native_server_new},
        {"serverFree", "(J)V", (void *)native_server_free},
        {"serverPhase", "(J)I", (void *)native_server_phase},
        {"serverInitialize", "(J[Z[I)[B", (void *)native_server_initialize},
        {"serverShutdown", "(J)I", (void *)native_server_shutdown},
        {"serverOpen", "(J[BI[B)I", (void *)native_server_open},
        {"serverUpdate", "(J[BI[I[B[I)I", (void *)native_server_update},
        {"serverClose", "(J[B)I", (void *)native_server_close},
        {"serverComplete", "(J[BII[J)I", (void *)native_server_complete},
    };
    JNIEnv *environment;
    jclass parser_class;
    (void)reserved;

    (*virtual_machine)->GetEnv(virtual_machine, (void **)&environment, JNI_VERSION_1_6);
    parser_class = (*environment)->FindClass(environment, "com/nouprax/markdown/core/JniParser");
    (*environment)->RegisterNatives(environment, parser_class, methods, (jint)(sizeof(methods) / sizeof(methods[0])));
    return JNI_VERSION_1_6;
}
