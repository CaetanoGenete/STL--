#include "gtest/gtest.h"

#include <vector>
#include <list>
#include <map>
#include <array>
#include <deque>
#include <set>
#include <unordered_map>
#include <concepts>

#include "named_requirements.hpp"

namespace smm_tests {
    
    // Expected bits represent, from most to  significant least:
    // container, reversible_container, allocator_aware_container, sequence_container
    template<typename Container, uint8_t Expected>
    struct container_requirements_test_pair {
        using container = Container;
        static constexpr uint8_t expected = Expected;
    };

    template<class Type>
    class container_requirements : public testing::TestWithParam<Type> {};
    
    using container_requirements_tests = testing::Types<
        container_requirements_test_pair<std::vector<int>            , 0b1111>,
        container_requirements_test_pair<std::deque<int>             , 0b1111>,
        container_requirements_test_pair<std::list<int>              , 0b1111>,
        container_requirements_test_pair<std::set<int>               , 0b1110>,
        container_requirements_test_pair<std::map<int, int>          , 0b1110>,
        container_requirements_test_pair<std::unordered_map<int, int>, 0b1010>
    >;
    TYPED_TEST_SUITE(container_requirements, container_requirements_tests);

    TYPED_TEST(container_requirements, concept_tests) {
        EXPECT_EQ(container<TypeParam::container>,                 static_cast<bool>(TypeParam::expected & 0b1000));
        EXPECT_EQ(reversible_container<TypeParam::container>,      static_cast<bool>(TypeParam::expected & 0b0100));
        EXPECT_EQ(allocator_aware_container<TypeParam::container>, static_cast<bool>(TypeParam::expected & 0b0010));
        //EXPECT_EQ(container<TypeParam::container>, static_cast<bool>(TypeParam::expected & 0b0001));
    }

}
