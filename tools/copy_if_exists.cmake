# cmake -DSRC=<file> -DDST=<file> -P copy_if_exists.cmake
if(EXISTS "${SRC}")
  file(COPY_FILE "${SRC}" "${DST}" ONLY_IF_DIFFERENT)
else()
  message(WARNING "${SRC} not built yet: run tools/build_asi_loader.ps1 (dist will lack the ASI loader)")
endif()
