"""Stage an isolated four-worker ESP32 capacity experiment; never edit the core."""
from pathlib import Path
import argparse
import shutil

parser = argparse.ArgumentParser()
parser.add_argument('destination', type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parents[3]
dest = args.destination.resolve()
dest.mkdir(parents=True, exist_ok=False)
shutil.copytree(root / 'graph_core', dest / 'graph_core')
p = dest / 'graph_core/include/kaos_admission.h'
s = p.read_text()
assert '#define CORE_MAX_NODE_RESERVATIONS 2' in s
assert '#define CORE_MAX_ADMISSION_RECORDS 4' in s
p.write_text(s.replace('#define CORE_MAX_NODE_RESERVATIONS 2', '#define CORE_MAX_NODE_RESERVATIONS 4').replace('#define CORE_MAX_ADMISSION_RECORDS 4', '#define CORE_MAX_ADMISSION_RECORDS 8'))
# These candidate layout limits only reserve RAM; this probe never submits
# larger requests or claims endpoint/report protocol conformance.
s = p.read_text().replace('#define CORE_MAX_ENDPOINT_RESERVATIONS 8', '#define CORE_MAX_ENDPOINT_RESERVATIONS 32').replace('#define CORE_MAX_REQUEST_CHANNELS 4', '#define CORE_MAX_REQUEST_CHANNELS 8').replace('#define CORE_MAX_PORTS 4', '#define CORE_MAX_PORTS 8')
p.write_text(s)
p = dest / 'graph_core/include/kaos_report.h'
p.write_text(p.read_text().replace('#define CORE_REPORT_MAX_OBJECTS 9', '#define CORE_REPORT_MAX_OBJECTS 17'))
project = dest / 'project'
main = project / 'main'
main.mkdir(parents=True)
shutil.copyfile(Path(__file__).with_name('probe.c'), main / 'probe.c')
shutil.copyfile(root / 'verification/graph_core/tests/runtime_guest.c', main / 'guest.c')
wamr = root.parent / 'sim_env/.deps/wamr'
project.joinpath('CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.18)
set(EXTRA_COMPONENT_DIRS "${{CMAKE_CURRENT_LIST_DIR}}/../graph_core" "${{CMAKE_CURRENT_LIST_DIR}}/../graph_core/runtime")
set(CORE_WAMR_ROOT "{wamr}" CACHE PATH "Pinned WAMR")
set(COMPONENTS main)
set(SDKCONFIG "${{CMAKE_BINARY_DIR}}/sdkconfig")
include($ENV{{IDF_PATH}}/tools/cmake/project.cmake)
project(kaos_capacity_experiment)
''')
shutil.copyfile(root / 'verification/graph_core/espidf_p3/sdkconfig.defaults', project / 'sdkconfig.defaults')
main.joinpath('CMakeLists.txt').write_text('''idf_component_register(SRCS "probe.c" REQUIRES runtime graph_core esp_timer)
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
  set(guest "${CMAKE_CURRENT_BINARY_DIR}/guest.wasm")
  add_custom_command(OUTPUT "${guest}"
    COMMAND /opt/wasi-sdk/bin/clang --target=wasm32 -O1 -nostdlib
      -Wl,--no-entry -Wl,--export=kaos_configure -Wl,--export=kaos_main
      -Wl,--export=__heap_base -Wl,--export=__data_end -Wl,--allow-undefined
      -Wl,--initial-memory=65536 -Wl,--max-memory=65536 -Wl,-z,stack-size=4096
      "${CMAKE_CURRENT_LIST_DIR}/guest.c" -o "${guest}"
    DEPENDS "${CMAKE_CURRENT_LIST_DIR}/guest.c" VERBATIM)
  add_custom_target(capacity_guest DEPENDS "${guest}")
  add_dependencies(${COMPONENT_LIB} capacity_guest)
  target_add_binary_data(${COMPONENT_LIB} "${guest}" BINARY)
endif()
''')
print(project)
