# Pipeline::Sequence - `sequence.pipeline` (tooling: the sequence asset, its builder and creator).
util_add_engine_library(Sequence.Pipeline ALIAS Pipeline::Sequence)
target_link_libraries(Sequence.Pipeline
    PUBLIC Foundation::Core Pipeline::Core Foundation::Sequence Foundation::Sequence.Resource
    PRIVATE Foundation::Policy)

target_sources(Sequence.Pipeline
    PRIVATE
        SequenceCreatorsImpl.cpp # File > New, asset_create
    PUBLIC
        FILE_SET sequence_pipeline_modules TYPE CXX_MODULES FILES
            SequenceAsset.cppm)
