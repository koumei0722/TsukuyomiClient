# CPM.cmake を取得して読み込むだけのラッパー。
# バージョンを固定しておき、取得済みならそのまま使う。

set(CPM_DOWNLOAD_VERSION 0.40.2)
set(CPM_DOWNLOAD_LOCATION "${CMAKE_BINARY_DIR}/cmake/CPM_${CPM_DOWNLOAD_VERSION}.cmake")

if(NOT EXISTS "${CPM_DOWNLOAD_LOCATION}")
    message(STATUS "CPM.cmake v${CPM_DOWNLOAD_VERSION} を取得します")
    file(DOWNLOAD
        "https://github.com/cpm-cmake/CPM.cmake/releases/download/v${CPM_DOWNLOAD_VERSION}/CPM.cmake"
        "${CPM_DOWNLOAD_LOCATION}"
        STATUS CPM_DOWNLOAD_STATUS)

    list(GET CPM_DOWNLOAD_STATUS 0 CPM_DOWNLOAD_CODE)
    if(NOT CPM_DOWNLOAD_CODE EQUAL 0)
        list(GET CPM_DOWNLOAD_STATUS 1 CPM_DOWNLOAD_MESSAGE)
        file(REMOVE "${CPM_DOWNLOAD_LOCATION}")
        message(FATAL_ERROR "CPM.cmake の取得に失敗しました: ${CPM_DOWNLOAD_MESSAGE}")
    endif()
endif()

include("${CPM_DOWNLOAD_LOCATION}")
