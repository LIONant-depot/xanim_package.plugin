include(${CMAKE_BINARY_DIR}/_deps/xcmake_tools/Common.cmake)

# Fetch dependencies
FetchAndPopulate("https://github.com/LIONant-depot/xbits.git" "main")
FetchAndPopulate("https://github.com/LIONant-depot/xserializer.git" "main")
FetchAndPopulate("https://github.com/LIONant-depot/xraw3d.git" "main")
FetchAndPopulate("https://github.com/LIONant-depot/xresource_pipeline_v2.git" "main")
FetchAndPopulate("https://github.com/LIONant-depot/xstrtool.git" "main" )
FetchAndPopulate("https://github.com/LIONant-depot/xscheduler.git" "main")


DefineInterfaceComponent(xanim_package_compiler_ "dependencies"
  "source/xanim_package_details.h"
  "source/xanim_package_descriptor.h"
  "source/xanim_package.h"
  "source/Compiler/xanim_package_compiler.cpp"
  "source/Compiler/xanim_package_compiler.h"
  "**XGPU"
  "source/xanim_package_xgpu_rsc_loader.h"
)
