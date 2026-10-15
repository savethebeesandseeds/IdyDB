{
  "variables": {
    "idydb_testing%": 0
  },
  "targets": [
    {
      "target_name": "idydb",
      "sources": [
        "addon.cpp",
        "../idydb/impl/db.cpp"
      ],
      "include_dirs": [
        "<!@(node -p \"require('node-addon-api').include\")",
        "../idydb/include"
      ],
      "dependencies": [
        "<!(node -p \"require('node-addon-api').gyp\")"
      ],
      "defines": [
        "NODE_ADDON_API_CPP_EXCEPTIONS",
        "NODE_ADDON_API_CPP_EXCEPTIONS_ALL"
      ],
      "cflags_cc": [ "-std=c++17" ],
      "cflags_cc!": [ "-fno-exceptions" ],

      "conditions": [
        ["idydb_testing==1", {
          "defines": [ "IDYDB_TESTING=1" ]
        }],
        ["OS!=\"win\"", {
          "sources": [ "../idydb/impl/platform_posix.cpp" ],
          "defines": [ "_FILE_OFFSET_BITS=64" ]
        }],
        ["OS==\"linux\"", {
          "libraries": [ "-lcrypto" ]
        }],

        ["OS==\"mac\"", {
          "xcode_settings": {
            "CLANG_CXX_LANGUAGE_STANDARD": "c++17",
            "GCC_ENABLE_CPP_EXCEPTIONS": "YES",
            "MACOSX_DEPLOYMENT_TARGET": "10.15"
          },
          "libraries": [ "-lcrypto" ],
          "include_dirs": [
            "<!@(node -p \"require('node-addon-api').include\")",
            "../idydb/include",
            "<!(node -p \"process.env.OPENSSL_INCLUDE_DIR || ''\")"
          ],
          "library_dirs": [
            "<!(node -p \"process.env.OPENSSL_LIB_DIR || ''\")"
          ]
        }],

        ["OS==\"win\"", {
          "sources": [ "../idydb/impl/platform_windows.cpp" ],
          "defines": [
            "NOMINMAX",
            "WIN32_LEAN_AND_MEAN"
          ],
          "msvs_settings": {
            "VCCLCompilerTool": {
              "LanguageStandard": "stdcpp17",
              "ExceptionHandling": 1,
              "RuntimeLibrary": 2,
              "AdditionalOptions": [
                "/std:c++17",
                "/utf-8"
              ]
            }
          },
          "include_dirs": [
            "<!@(node -p \"require('node-addon-api').include\")",
            "../idydb/include",
            "<!(node -p \"process.env.OPENSSL_INCLUDE_DIR || ''\")"
          ],
          "library_dirs": [
            "<!(node -p \"process.env.OPENSSL_LIB_DIR || ''\")"
          ],
          "libraries": [
            "libcrypto.lib",
            "crypt32.lib",
            "ws2_32.lib",
            "advapi32.lib",
            "gdi32.lib"
          ]
        }]
      ]
    }
  ]
}
