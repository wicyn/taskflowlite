#include "../taskflowlite/taskflowlite.hpp"
#include <iostream>
#include <fstream>

int main() {
    std::cout << "=== Example 10: D2 Dump (Visualization) ===\n";
    
    tfl::Flow flow;
    flow.name("MyPipeline");
    
    auto input = flow.emplace([] { std::cout << "Reading data\n"; });
    input.name("Input");
    
    auto process1 = flow.emplace([] { std::cout << "Processing step 1\n"; });
    process1.name("Process1");
    
    auto process2a = flow.emplace([] { std::cout << "Processing step 2a\n"; });
    process2a.name("Process2A");
    
    auto process2b = flow.emplace([] { std::cout << "Processing step 2b\n"; });
    process2b.name("Process2B");
    
    auto output = flow.emplace([] { std::cout << "Writing output\n"; });
    output.name("Output");
    
    input.precede(process1);
    process1.precede(process2a, process2b);
    process2a.precede(output);
    process2b.precede(output);
    
    std::cout << "Dumping flow to 'pipeline.d2'...\n";
    
    std::ofstream file("pipeline.d2");
    flow.dump(file);
    file.close();
    
    std::cout << "\nD2 content:\n";
    std::cout << "==========\n";
    auto dump = flow.dump();
    std::cout << dump;
    std::cout << "==========\n";
    
    std::cout << "\nDump example complete!\n";
    std::cout << "To visualize, copy pipeline.d2 content to https://play.d2lang.com\n";
    
    return 0;
}
