fc_add_catch_test(fc_abi_contract_tests
    NAME fc_abi_contract
    LABEL fast
    SOURCES
        abi_c_contract.c
        abi_cpp_contract.cpp
        query_definitions.c
)

# C11 compilation is intentional: public ABI headers must remain valid without C++ language support.
set_target_properties(fc_abi_contract_tests PROPERTIES
    C_EXTENSIONS OFF
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
)

target_include_directories(fc_abi_contract_tests PRIVATE
    "${FC_SOURCE_ROOT}/include"
)
