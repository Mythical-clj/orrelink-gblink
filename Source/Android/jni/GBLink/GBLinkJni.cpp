// GB-Link on Android: the bridge between the Kotlin USB layer / launcher card and the shared
// GB-Link manager.
#include <jni.h>

#include "InputCommon/GBLink/GBLinkManager.h"
#include "InputCommon/GBLink/UsbLink.h"

extern "C" {

// Called once from GBLinkUsb (Kotlin) with its own class, so the native USB code can call back
// into it from any thread, then starts the adapter manager.
JNIEXPORT void JNICALL Java_org_dolphinemu_dolphinemu_utils_GBLinkUsb_nativeInit(JNIEnv* env,
                                                                                  jclass cls)
{
  GBLink::AndroidBridge::Init(env, cls);
  GBLink::Manager::Get().Start();
}

JNIEXPORT jstring JNICALL
Java_org_dolphinemu_dolphinemu_features_xdnetplay_GBLinkBridge_getStatus(JNIEnv* env, jclass)
{
  return env->NewStringUTF(GBLink::Manager::Get().GetStatus().text.c_str());
}

JNIEXPORT jint JNICALL
Java_org_dolphinemu_dolphinemu_features_xdnetplay_GBLinkBridge_getAdapterCount(JNIEnv*, jclass)
{
  return static_cast<jint>(GBLink::Manager::Get().GetAdapters().size());
}

JNIEXPORT void JNICALL
Java_org_dolphinemu_dolphinemu_features_xdnetplay_GBLinkBridge_setControllerEnabled(JNIEnv*, jclass,
                                                                                    jboolean on)
{
  GBLink::Manager::Get().SetControllerEnabled(on);
}

JNIEXPORT jboolean JNICALL
Java_org_dolphinemu_dolphinemu_features_xdnetplay_GBLinkBridge_isControllerEnabled(JNIEnv*, jclass)
{
  return GBLink::Manager::Get().IsControllerEnabled();
}

JNIEXPORT void JNICALL
Java_org_dolphinemu_dolphinemu_features_xdnetplay_GBLinkBridge_setScreenMirror(JNIEnv*, jclass,
                                                                               jboolean on)
{
  GBLink::Manager::Get().SetScreenMirror(on);
}

JNIEXPORT jboolean JNICALL
Java_org_dolphinemu_dolphinemu_features_xdnetplay_GBLinkBridge_isScreenMirror(JNIEnv*, jclass)
{
  return GBLink::Manager::Get().IsScreenMirror();
}

}  // extern "C"
