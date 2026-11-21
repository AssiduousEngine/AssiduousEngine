# Foundation::Sequence - `foundation.sequence`: cutscene timelines. The model (tracks bound by name,
# markers and holds), the play that runs one on a scene, the track kind base and the kinds that need
# no domain (property and event tracks). Domains add their kinds; nothing here knows rendering,
# audio or skeletal animation.
util_add_engine_library(Sequence ALIAS Foundation::Sequence)
target_link_libraries(Sequence
    PUBLIC Foundation::Core Foundation::Scene Foundation::PropertyAnimation
    PRIVATE Foundation::Policy)

target_sources(Sequence
    PRIVATE
        SequenceImpl.cpp
    PUBLIC
        FILE_SET sequence_modules TYPE CXX_MODULES FILES
            SequenceModule.cppm)
