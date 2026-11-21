# Foundation::PropertyAnimation - `foundation.propertyanimation`: the clip/track/curve data model,
# its evaluation, the reflection binding resolver and track targets on a scene. Not on
# foundation.animation, so a headless consumer evaluates a property clip without skeletal machinery.
util_add_engine_library(PropertyAnimation ALIAS Foundation::PropertyAnimation)
target_link_libraries(PropertyAnimation
    PUBLIC Foundation::Core Foundation::Scene
    PRIVATE Foundation::Policy)

target_sources(PropertyAnimation
    PRIVATE
        PropertyAnimationImpl.cpp
    PUBLIC
        FILE_SET propertyanimation_modules TYPE CXX_MODULES FILES
            PropertyAnimationModule.cppm)
