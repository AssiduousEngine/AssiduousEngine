# Foundation::Sequence.Resource - `foundation.sequence.resource`: the cooked sequence as a runtime
# resource (a SequenceSource, through its factory, becomes a SequenceResource), and the nested
# sequence track, which refers to one.
util_add_engine_library(Sequence.Resource ALIAS Foundation::Sequence.Resource)
target_link_libraries(Sequence.Resource
    PUBLIC Foundation::Core Foundation::Resource Foundation::Content Foundation::Scene Foundation::Sequence
    PRIVATE Foundation::Policy)

target_sources(Sequence.Resource
    PRIVATE
        SequenceResourceImpl.cpp
    PUBLIC
        FILE_SET sequence_resource_modules TYPE CXX_MODULES FILES
            SequenceResource.cppm)
