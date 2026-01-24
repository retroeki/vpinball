#include <iostream>
#include <regex>
#include <string>

int main() {
    // Fixed: [ \t]+ instead of \s+ to not match newlines
    std::regex dotAccess(R"(\b(\w+)\.(\w+)(\s*\(([^)]*)\)|[ \t]+([^'=:\r\n\s][^:\r\n]*))?)", std::regex::icase);
    
    // Test 1: Method with comment (should NOT capture comment)
    std::string t1 = "LF.Fire  'comment";
    std::smatch m;
    std::cout << "Test 1: [" << t1 << "]" << std::endl;
    if (std::regex_search(t1, m, dotAccess)) {
        std::cout << "  Match: [" << m[0].str() << "]" << std::endl;
        std::cout << "  Group 5 matched: " << m[5].matched << std::endl;
    }
    
    // Test 2: Method with args (should capture args)
    std::string t2 = "LF.ReProcessBalls ActiveBall";
    std::cout << "\nTest 2: [" << t2 << "]" << std::endl;
    if (std::regex_search(t2, m, dotAccess)) {
        std::cout << "  Match: [" << m[0].str() << "]" << std::endl;
        std::cout << "  Group 5: [" << m[5].str() << "]" << std::endl;
    }
    
    // Test 3: Method followed by newline (should NOT capture across newline)
    std::string t3 = "cor.Update\nEnd Sub";
    std::cout << "\nTest 3: [cor.Update\nEnd Sub]" << std::endl;
    if (std::regex_search(t3, m, dotAccess)) {
        std::cout << "  Match: [" << m[0].str() << "]" << std::endl;
        std::cout << "  Group 5 matched: " << m[5].matched << std::endl;
        if (m[5].matched) std::cout << "  Group 5: [" << m[5].str() << "]" << std::endl;
    }
    
    return 0;
}
