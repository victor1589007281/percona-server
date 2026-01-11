# Aurora Plugin CMakeLists.txt
# Build configuration for Aurora distributed storage plugin
#
# Reference: 01_compute_layer.md Section 11.8

CMAKE_MINIMUM_REQUIRED(VERSION 3.14)

# Aurora Plugin Option
OPTION(WITH_AURORA "Build with Aurora distributed storage support" OFF)

IF(WITH_AURORA)
    MESSAGE(STATUS "Aurora: Enabled")
    ADD_DEFINITIONS(-DHAVE_AURORA)
    
    # ==================== Source Files ====================
    SET(AURORA_CORE_SOURCES
        aurora.cc
        aurora_hook.cc
        aurora_config.cc
        aurora_startup.cc
        aurora_plugin.cc
    )
    
    SET(AURORA_CLIENT_SOURCES
        aurora_client.cc
        aurora_redo_sender.cc
        aurora_page_reader.cc
        aurora_reader_sync.cc
    )
    
    SET(AURORA_BINLOG_SOURCES
        aurora_gtid.cc
        aurora_binlog.cc
    )
    
    SET(AURORA_COMMAND_SOURCES
        aurora_commands.cc
        aurora_sysvars.cc
    )
    
    SET(AURORA_REPLICATION_SOURCES
        aurora_replication.cc
    )
    
    SET(AURORA_OLAP_SOURCES
        aurora_olap_router.cc
    )
    
    # All Aurora sources
    SET(AURORA_ALL_SOURCES
        ${AURORA_CORE_SOURCES}
        ${AURORA_CLIENT_SOURCES}
        ${AURORA_BINLOG_SOURCES}
        ${AURORA_COMMAND_SOURCES}
        ${AURORA_REPLICATION_SOURCES}
        ${AURORA_OLAP_SOURCES}
    )
    
    # ==================== Headers ====================
    SET(AURORA_HEADERS
        aurora.h
        aurora_hook.h
        aurora_config.h
        aurora_types.h
        aurora_redo_types.h
        aurora_client.h
        aurora_redo_sender.h
        aurora_page_reader.h
        aurora_reader_sync.h
        aurora_gtid.h
        aurora_binlog.h
        aurora_commands.h
        aurora_sysvars.h
        aurora_replication.h
        aurora_transport.h
        aurora_startup.h
        aurora_plugin.h
        aurora_integration.h
        aurora_olap_router.h
    )
    
    # ==================== Create Library ====================
    ADD_LIBRARY(aurora STATIC ${AURORA_ALL_SOURCES})
    
    TARGET_INCLUDE_DIRECTORIES(aurora PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/..
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    
    # C++17 required
    SET_TARGET_PROPERTIES(aurora PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
    
    # ==================== gRPC/Protobuf (Optional) ====================
    # Uncomment when gRPC is available
    #
    # FIND_PACKAGE(gRPC CONFIG REQUIRED)
    # FIND_PACKAGE(Protobuf CONFIG REQUIRED)
    #
    # SET(PROTO_FILES
    #     proto/storage.proto
    #     proto/metadata.proto
    #     proto/compute.proto
    # )
    #
    # PROTOBUF_GENERATE_CPP(PROTO_SRCS PROTO_HDRS ${PROTO_FILES})
    # GRPC_GENERATE_CPP(GRPC_SRCS GRPC_HDRS ${PROTO_FILES})
    #
    # TARGET_SOURCES(aurora PRIVATE ${PROTO_SRCS} ${GRPC_SRCS})
    # TARGET_LINK_LIBRARIES(aurora
    #     gRPC::grpc++
    #     protobuf::libprotobuf
    # )
    
    # ==================== Export for InnoDB ====================
    # This will be linked into InnoDB
    SET(AURORA_LIBRARY aurora PARENT_SCOPE)
    SET(AURORA_SOURCES ${AURORA_ALL_SOURCES} PARENT_SCOPE)
    SET(AURORA_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR} PARENT_SCOPE)
    
    # ==================== Installation ====================
    INSTALL(FILES ${AURORA_HEADERS}
        DESTINATION include/mysql/storage/innobase/aurora
    )
    
    INSTALL(TARGETS aurora
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib
    )
    
ELSE()
    MESSAGE(STATUS "Aurora: Disabled")
ENDIF()
