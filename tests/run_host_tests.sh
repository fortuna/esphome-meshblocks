#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
meshcore_openssl_prefix=${OPENSSL_PREFIX:-$(brew --prefix openssl@3)}
mkdir -p build/host-tests
"${CXX:-clang++}" -std=c++17 -Wall -Wextra -Werror -g \
  -fsanitize=address,undefined -fno-omit-frame-pointer -Icomponents/meshcore_core \
  tests/test_epoch_clock.cpp -o build/host-tests/epoch_clock_test
build/host-tests/epoch_clock_test
mkdir -p build/host-tests/include/esphome/components
ln -sfn "$PWD/components/meshcore_core" build/host-tests/include/esphome/components/meshcore_core
ln -sfn "$PWD/components/meshcore" build/host-tests/include/esphome/components/meshcore
"${CXX:-clang++}" -std=c++17 -Wall -Wextra -Werror -g \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Ibuild/host-tests/include -Icomponents/meshcore \
  tests/test_channel_state.cpp -o build/host-tests/channel_state_test
build/host-tests/channel_state_test
"${CXX:-clang++}" -std=c++17 -Wall -Wextra -Werror -g \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Icomponents/meshcore_core -I"$meshcore_openssl_prefix/include" \
  components/meshcore_core/channel_codec.cpp tests/test_channel_codec.cpp \
  -L"$meshcore_openssl_prefix/lib" -Wl,-rpath,"$meshcore_openssl_prefix/lib" -lcrypto \
  -o build/host-tests/channel_codec_test
build/host-tests/channel_codec_test
"${CXX:-clang++}" -std=c++17 -Wall -Wextra -Werror -g \
  -fsanitize=address,undefined -fno-omit-frame-pointer -Icomponents/meshcore_core \
  components/meshcore_core/radio_scheduler.cpp tests/test_radio_scheduler.cpp \
  -o build/host-tests/radio_scheduler_test
build/host-tests/radio_scheduler_test
"${CXX:-clang++}" -std=c++17 -Wall -Wextra -Werror -g \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Itests/fakes -Ibuild/host-tests/include -Icomponents/meshcore \
  components/meshcore/sx126x_radio.cpp tests/test_sx126x_radio.cpp \
  -o build/host-tests/sx126x_radio_test
build/host-tests/sx126x_radio_test
for meshcore_c_file in components/meshcore_ed25519/*.c; do
  meshcore_c_name=$(basename "$meshcore_c_file" .c)
  "${CC:-clang}" -std=c99 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
    -c "$meshcore_c_file" -o "build/host-tests/ed_$meshcore_c_name.o"
done
"${CXX:-clang++}" -std=c++17 -Wall -Wextra -Werror -g \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Icomponents/meshcore_core -I"$meshcore_openssl_prefix/include" \
  components/meshcore_core/channel_codec.cpp components/meshcore_core/room_protocol.cpp components/meshcore_core/room_service.cpp \
  components/meshcore/ed25519_identity.cpp tests/test_room.cpp build/host-tests/ed_*.o \
  -L"$meshcore_openssl_prefix/lib" -Wl,-rpath,"$meshcore_openssl_prefix/lib" -lcrypto \
  -o build/host-tests/room_test
UBSAN_OPTIONS=halt_on_error=1 build/host-tests/room_test
"${CXX:-clang++}" -std=c++17 -Wall -Wextra -Werror -g \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Icomponents/meshcore_core -I"$meshcore_openssl_prefix/include" \
  components/meshcore_core/channel_codec.cpp components/meshcore_core/room_protocol.cpp components/meshcore_core/room_service.cpp \
  components/meshcore_core/client_service.cpp components/meshcore_core/node_runtime.cpp components/meshcore_core/radio_scheduler.cpp \
  components/meshcore/ed25519_identity.cpp tests/test_node_runtime.cpp build/host-tests/ed_*.o \
  -L"$meshcore_openssl_prefix/lib" -Wl,-rpath,"$meshcore_openssl_prefix/lib" -lcrypto \
  -o build/host-tests/node_runtime_test
UBSAN_OPTIONS=halt_on_error=1 build/host-tests/node_runtime_test
"${CXX:-clang++}" -std=c++17 -Wall -Wextra -Werror -g \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Itests/fakes -Icomponents/meshcore_core -I"$meshcore_openssl_prefix/include" \
  components/meshcore_core/channel_codec.cpp components/meshcore_core/room_protocol.cpp components/meshcore_core/room_service.cpp \
  components/meshcore_core/client_service.cpp components/meshcore_core/node_runtime.cpp components/meshcore/ed25519_identity.cpp \
  components/meshcore/nvs_room_store.cpp components/meshcore/room_runtime.cpp \
  tests/test_static_rooms.cpp build/host-tests/ed_*.o \
  -L"$meshcore_openssl_prefix/lib" -Wl,-rpath,"$meshcore_openssl_prefix/lib" -lcrypto \
  -o build/host-tests/static_rooms_test
UBSAN_OPTIONS=halt_on_error=1 build/host-tests/static_rooms_test
"${CXX:-clang++}" -std=c++17 -Wall -Wextra -Werror -g \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Icomponents/meshcore_core -I"$meshcore_openssl_prefix/include" \
  components/meshcore_core/channel_codec.cpp components/meshcore_core/room_protocol.cpp components/meshcore_core/room_service.cpp \
  components/meshcore_core/client_service.cpp components/meshcore_core/node_runtime.cpp components/meshcore_core/radio_scheduler.cpp \
  components/meshcore/ed25519_identity.cpp tests/test_client_service.cpp build/host-tests/ed_*.o \
  -L"$meshcore_openssl_prefix/lib" -Wl,-rpath,"$meshcore_openssl_prefix/lib" -lcrypto \
  -o build/host-tests/client_service_test
UBSAN_OPTIONS=halt_on_error=1 build/host-tests/client_service_test
