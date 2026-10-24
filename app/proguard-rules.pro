# Release shrinking is off by default. These rules keep the JNI surface intact if it is ever switched on.

# Every native method keeps its exact name (the C++ side exports Java_com_pitchbot_app_Native_*).
-keepclasseswithmembernames class * {
    native <methods>;
}

# The class that declares them, and the services Android binds by name.
-keep class com.pitchbot.app.Native { *; }
-keep class com.pitchbot.app.BotService { *; }
-keep class com.pitchbot.app.TapService { *; }
