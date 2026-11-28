find_package(${QT_PACKAGE_NAME} ${DP_MIN_QT_VERSION_GUI} REQUIRED COMPONENTS Test)

add_unit_tests(client
	LIBS dpclient ${QT_PACKAGE_NAME}::Test
	TESTS html kritabundle listingfiltering news
)

file(TO_CMAKE_PATH "${PROJECT_SOURCE_DIR}/src/desktop/assets" DP_BRUSH_ASSET_DIR)
target_compile_definitions(test_client_kritabundle PRIVATE
	DP_BRUSH_ASSET_DIR="${DP_BRUSH_ASSET_DIR}"
)
