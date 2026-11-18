# Built into the existing cooperative-session archive, including the runtime
# kit. Internet support doesn't add a separately launched helper program.
set(INVITE_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/port/src/coop)
set(INVITE_JUICE ${CMAKE_CURRENT_SOURCE_DIR}/vendor/libjuice)
set(INVITE_KCP ${CMAKE_CURRENT_SOURCE_DIR}/vendor/kcp)
target_sources(ssne-coop PRIVATE
  ${INVITE_SOURCE}/invite_code.cpp ${INVITE_SOURCE}/invite_signalling.cpp
  ${INVITE_SOURCE}/invite_transport.cpp ${INVITE_SOURCE}/invite_upnp.cpp
  ${INVITE_SOURCE}/invite_settings.cpp launcher/src/launcher_json.cpp
  ${INVITE_KCP}/ikcp.c
  ${INVITE_JUICE}/src/addr.c ${INVITE_JUICE}/src/agent.c
  ${INVITE_JUICE}/src/crc32.c ${INVITE_JUICE}/src/const_time.c
  ${INVITE_JUICE}/src/conn.c ${INVITE_JUICE}/src/conn_poll.c
  ${INVITE_JUICE}/src/conn_thread.c ${INVITE_JUICE}/src/conn_mux.c
  ${INVITE_JUICE}/src/base64.c ${INVITE_JUICE}/src/hash.c
  ${INVITE_JUICE}/src/hmac.c ${INVITE_JUICE}/src/ice.c
  ${INVITE_JUICE}/src/juice.c ${INVITE_JUICE}/src/log.c
  ${INVITE_JUICE}/src/random.c ${INVITE_JUICE}/src/server.c
  ${INVITE_JUICE}/src/stun.c ${INVITE_JUICE}/src/timestamp.c
  ${INVITE_JUICE}/src/tcp.c ${INVITE_JUICE}/src/turn.c
  ${INVITE_JUICE}/src/udp.c)
target_include_directories(ssne-coop PRIVATE
  ${INVITE_JUICE}/include ${INVITE_JUICE}/include/juice
  ${INVITE_JUICE}/src ${INVITE_KCP})
target_compile_definitions(ssne-coop PRIVATE JUICE_STATIC USE_NETTLE=0 NO_SERVER)
option(SSNE_INVITE_LOOPBACK_TEST "Allow localhost ICE candidates in private test builds" OFF)
if(SSNE_INVITE_LOOPBACK_TEST)
  target_compile_definitions(ssne-coop PRIVATE JUICE_ENABLE_LOCALHOST_ADDRESS=1)
endif()
target_link_libraries(ssne-coop PUBLIC winhttp xmllite ole32)
