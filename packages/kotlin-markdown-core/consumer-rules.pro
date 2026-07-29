# JNI resolves this one private bridge by binary class and method name.
# Preserve only that linkage boundary; every other implementation class and
# member remains fully shrinkable, optimizable, and obfuscatable. The bridge
# itself is also removable when the consuming application never uses it.
-keepclasseswithmembernames,allowoptimization class com.nouprax.markdown.core.JvmNative {
    native <methods>;
}
