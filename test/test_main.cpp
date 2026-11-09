#include <gtest/gtest.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include <QLoggingCategory>
#include <filesystem>

int main(int argc, char *argv[]) 
{
  // Fixtures are opened as "../examples/...", relative to the source test/ folder (set by
  // test/CMakeLists.txt). vm_state output is unaffected: its path is fixed before main runs.
  std::filesystem::current_path(KITES_TEST_SOURCE_DIR);
  #ifdef _WIN32
    // Set console output code page to UTF-8 (65001)
    SetConsoleOutputCP(65001);  
  #endif
  ::testing::InitGoogleTest(&argc, argv);
  //qInstallMessageHandler([](QtMsgType, const QMessageLogContext&, const QString&) {});
  //std::cout.setstate(std::ios_base::failbit);

  return RUN_ALL_TESTS();

}