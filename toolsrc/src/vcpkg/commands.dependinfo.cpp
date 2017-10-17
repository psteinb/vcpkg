#include "pch.h"

#include <vcpkg/base/strings.h>
#include <vcpkg/base/system.h>
#include <vcpkg/base/util.h>
#include <vcpkg/commands.h>
#include <vcpkg/help.h>
#include <vcpkg/paragraphs.h>

namespace vcpkg::Commands::DependInfo
{
    void perform_and_exit(const VcpkgCmdArguments& args, const VcpkgPaths& paths)
    {
        static const std::string EXAMPLE = Help::create_example_string(R"###(depend-info [pat])###");
        args.check_max_arg_count(1, EXAMPLE);
        args.check_and_get_optional_command_arguments({});

        std::vector<std::unique_ptr<SourceControlFile>> source_control_files =
            Paragraphs::load_all_ports(paths.get_filesystem(), paths.ports);

        if (args.command_arguments.size() == 1)
        {
            const std::string filter = args.command_arguments.at(0);

            Util::erase_remove_if(source_control_files,
                                  [&](const std::unique_ptr<SourceControlFile>& source_control_file) {

                                      const SourceParagraph& source_paragraph = *source_control_file->core_paragraph;

                                      if (Strings::case_insensitive_ascii_contains(source_paragraph.name, filter))
                                      {
                                          return false;
                                      }

                                      for (const Dependency& dependency : source_paragraph.depends)
                                      {
                                          if (Strings::case_insensitive_ascii_contains(dependency.name(), filter))
                                          {
                                              return false;
                                          }
                                      }

                                      return true;
                                  });
        }

        for (auto&& source_control_file : source_control_files)
        {
            const SourceParagraph& source_paragraph = *source_control_file->core_paragraph;
            const auto s = Strings::join(", ", source_paragraph.depends, [](const Dependency& d) { return d.name(); });
            System::println("%s: %s", source_paragraph.name, s);
        }

        Checks::exit_success(VCPKG_LINE_INFO);
    }
}
