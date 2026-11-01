sigfm_sources = ['sigfm.cpp']

opencv = dependency('opencv4', 'opencv5', required: true, version: '>=4.5.0')

libsigfm = static_library('sigfm',
        sigfm_sources,
        dependencies: [opencv],
)
