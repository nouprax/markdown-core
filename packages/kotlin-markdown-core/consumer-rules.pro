# JNI_OnLoad registers this exact class and method name with RegisterNatives.
# Keep only their names; ordinary reachability analysis may still remove the
# adapter when the Markdown Core API is unused.
-keepclasseswithmembernames,allowoptimization class com.nouprax.markdown.core.JniParser {
    native <methods>;
}
