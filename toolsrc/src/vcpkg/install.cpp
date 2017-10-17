#include "pch.h"

#include <vcpkg/base/files.h>
#include <vcpkg/base/system.h>
#include <vcpkg/base/util.h>
#include <vcpkg/build.h>
#include <vcpkg/commands.h>
#include <vcpkg/dependencies.h>
#include <vcpkg/globalstate.h>
#include <vcpkg/help.h>
#include <vcpkg/input.h>
#include <vcpkg/install.h>
#include <vcpkg/metrics.h>
#include <vcpkg/paragraphs.h>
#include <vcpkg/remove.h>
#include <vcpkg/vcpkglib.h>

namespace vcpkg::Install
{
    using namespace Dependencies;

    InstallDir InstallDir::from_destination_root(const fs::path& destination_root,
                                                 const std::string& destination_subdirectory,
                                                 const fs::path& listfile)
    {
        InstallDir dirs;
        dirs.m_destination = destination_root / destination_subdirectory;
        dirs.m_destination_subdirectory = destination_subdirectory;
        dirs.m_listfile = listfile;
        return dirs;
    }

    const fs::path& InstallDir::destination() const { return this->m_destination; }

    const std::string& InstallDir::destination_subdirectory() const { return this->m_destination_subdirectory; }

    const fs::path& InstallDir::listfile() const { return this->m_listfile; }

    void install_files_and_write_listfile(Files::Filesystem& fs,
                                          const fs::path& source_dir,
                                          const InstallDir& destination_dir)
    {
        std::vector<std::string> output;
        std::error_code ec;

        const size_t prefix_length = source_dir.native().size();
        const fs::path& destination = destination_dir.destination();
        const std::string& destination_subdirectory = destination_dir.destination_subdirectory();
        const fs::path& listfile = destination_dir.listfile();

        Checks::check_exit(
            VCPKG_LINE_INFO, fs.exists(source_dir), "Source directory %s does not exist", source_dir.generic_string());
        fs.create_directories(destination, ec);
        Checks::check_exit(
            VCPKG_LINE_INFO, !ec, "Could not create destination directory %s", destination.generic_string());
        const fs::path listfile_parent = listfile.parent_path();
        fs.create_directories(listfile_parent, ec);
        Checks::check_exit(
            VCPKG_LINE_INFO, !ec, "Could not create directory for listfile %s", listfile.generic_string());

        output.push_back(Strings::format(R"(%s/)", destination_subdirectory));
        auto files = fs.get_files_recursive(source_dir);
        for (auto&& file : files)
        {
            const auto status = fs.status(file, ec);
            if (ec)
            {
                System::println(System::Color::error, "failed: %s: %s", file.u8string(), ec.message());
                continue;
            }

            const std::string filename = file.filename().generic_string();
            if (fs::is_regular_file(status) && (Strings::case_insensitive_ascii_equals(filename.c_str(), "CONTROL") ||
                                                Strings::case_insensitive_ascii_equals(filename.c_str(), "BUILD_INFO")))
            {
                // Do not copy the control file
                continue;
            }

            const std::string suffix = file.generic_u8string().substr(prefix_length + 1);
            const fs::path target = destination / suffix;

            if (fs::is_directory(status))
            {
                fs.create_directory(target, ec);
                if (ec)
                {
                    System::println(System::Color::error, "failed: %s: %s", target.u8string(), ec.message());
                }

                // Trailing backslash for directories
                output.push_back(Strings::format(R"(%s/%s/)", destination_subdirectory, suffix));
                continue;
            }

            if (fs::is_regular_file(status))
            {
                if (fs.exists(target))
                {
                    System::println(System::Color::warning,
                                    "File %s was already present and will be overwritten",
                                    target.u8string(),
                                    ec.message());
                }
                fs.copy_file(file, target, fs::copy_options::overwrite_existing, ec);
                if (ec)
                {
                    System::println(System::Color::error, "failed: %s: %s", target.u8string(), ec.message());
                }
                output.push_back(Strings::format(R"(%s/%s)", destination_subdirectory, suffix));
                continue;
            }

            if (!fs::status_known(status))
            {
                System::println(System::Color::error, "failed: %s: unknown status", file.u8string());
                continue;
            }

            System::println(System::Color::error, "failed: %s: cannot handle file type", file.u8string());
        }

        std::sort(output.begin(), output.end());

        fs.write_lines(listfile, output);
    }

    static void remove_first_n_chars(std::vector<std::string>* strings, const size_t n)
    {
        for (std::string& s : *strings)
        {
            s.erase(0, n);
        }
    };

    static std::vector<std::string> extract_files_in_triplet(
        const std::vector<StatusParagraphAndAssociatedFiles>& pgh_and_files, const Triplet& triplet)
    {
        std::vector<std::string> output;
        for (const StatusParagraphAndAssociatedFiles& t : pgh_and_files)
        {
            if (t.pgh.package.spec.triplet() != triplet)
            {
                continue;
            }

            Util::Vectors::concatenate(&output, t.files);
        }

        std::sort(output.begin(), output.end());
        return output;
    }

    static SortedVector<std::string> build_list_of_package_files(const Files::Filesystem& fs,
                                                                 const fs::path& package_dir)
    {
        const std::vector<fs::path> package_file_paths = fs.get_files_recursive(package_dir);
        const size_t package_remove_char_count = package_dir.generic_string().size() + 1; // +1 for the slash
        auto package_files = Util::fmap(package_file_paths, [package_remove_char_count](const fs::path& path) {
            std::string as_string = path.generic_string();
            as_string.erase(0, package_remove_char_count);
            return std::move(as_string);
        });

        return SortedVector<std::string>(std::move(package_files));
    }

    static SortedVector<std::string> build_list_of_installed_files(
        const std::vector<StatusParagraphAndAssociatedFiles>& pgh_and_files, const Triplet& triplet)
    {
        std::vector<std::string> installed_files = extract_files_in_triplet(pgh_and_files, triplet);
        const size_t installed_remove_char_count = triplet.canonical_name().size() + 1; // +1 for the slash
        remove_first_n_chars(&installed_files, installed_remove_char_count);

        return SortedVector<std::string>(std::move(installed_files));
    }

    InstallResult install_package(const VcpkgPaths& paths, const BinaryControlFile& bcf, StatusParagraphs* status_db)
    {
        const fs::path package_dir = paths.package_dir(bcf.core_paragraph.spec);
        const Triplet& triplet = bcf.core_paragraph.spec.triplet();
        const std::vector<StatusParagraphAndAssociatedFiles> pgh_and_files = get_installed_files(paths, *status_db);

        const SortedVector<std::string> package_files =
            build_list_of_package_files(paths.get_filesystem(), package_dir);
        const SortedVector<std::string> installed_files = build_list_of_installed_files(pgh_and_files, triplet);

        std::vector<std::string> intersection;
        std::set_intersection(package_files.begin(),
                              package_files.end(),
                              installed_files.begin(),
                              installed_files.end(),
                              std::back_inserter(intersection));

        if (!intersection.empty())
        {
            const fs::path triplet_install_path = paths.installed / triplet.canonical_name();
            System::println(System::Color::error,
                            "The following files are already installed in %s and are in conflict with %s",
                            triplet_install_path.generic_string(),
                            bcf.core_paragraph.spec);
            System::print("\n    ");
            System::println(Strings::join("\n    ", intersection));
            System::println();
            return InstallResult::FILE_CONFLICTS;
        }

        StatusParagraph source_paragraph;
        source_paragraph.package = bcf.core_paragraph;
        source_paragraph.want = Want::INSTALL;
        source_paragraph.state = InstallState::HALF_INSTALLED;

        write_update(paths, source_paragraph);
        status_db->insert(std::make_unique<StatusParagraph>(source_paragraph));

        std::vector<StatusParagraph> features_spghs;
        for (auto&& feature : bcf.features)
        {
            features_spghs.emplace_back();

            StatusParagraph& feature_paragraph = features_spghs.back();
            feature_paragraph.package = feature;
            feature_paragraph.want = Want::INSTALL;
            feature_paragraph.state = InstallState::HALF_INSTALLED;

            write_update(paths, feature_paragraph);
            status_db->insert(std::make_unique<StatusParagraph>(feature_paragraph));
        }

        const InstallDir install_dir = InstallDir::from_destination_root(
            paths.installed, triplet.to_string(), paths.listfile_path(bcf.core_paragraph));

        install_files_and_write_listfile(paths.get_filesystem(), package_dir, install_dir);

        source_paragraph.state = InstallState::INSTALLED;
        write_update(paths, source_paragraph);
        status_db->insert(std::make_unique<StatusParagraph>(source_paragraph));

        for (auto&& feature_paragraph : features_spghs)
        {
            feature_paragraph.state = InstallState::INSTALLED;
            write_update(paths, feature_paragraph);
            status_db->insert(std::make_unique<StatusParagraph>(feature_paragraph));
        }

        return InstallResult::SUCCESS;
    }

    using Build::BuildResult;

    BuildResult perform_install_plan_action(const VcpkgPaths& paths,
                                            const InstallPlanAction& action,
                                            const Build::BuildPackageOptions& build_package_options,
                                            StatusParagraphs& status_db)
    {
        const InstallPlanType& plan_type = action.plan_type;
        const std::string display_name = action.spec.to_string();
        const std::string display_name_with_features =
            GlobalState::feature_packages ? action.displayname() : display_name;

        const bool is_user_requested = action.request_type == RequestType::USER_REQUESTED;
        const bool use_head_version = to_bool(build_package_options.use_head_version);

        if (plan_type == InstallPlanType::ALREADY_INSTALLED)
        {
            if (use_head_version && is_user_requested)
                System::println(
                    System::Color::warning, "Package %s is already installed -- not building from HEAD", display_name);
            else
                System::println(System::Color::success, "Package %s is already installed", display_name);
            return BuildResult::SUCCEEDED;
        }

        if (plan_type == InstallPlanType::BUILD_AND_INSTALL)
        {
            if (use_head_version)
                System::println("Building package %s from HEAD... ", display_name_with_features);
            else
                System::println("Building package %s... ", display_name_with_features);

            const auto result = [&]() -> Build::ExtendedBuildResult {
                if (GlobalState::feature_packages)
                {
                    const Build::BuildPackageConfig build_config{
                        *action.any_paragraph.source_control_file.value_or_exit(VCPKG_LINE_INFO),
                        action.spec.triplet(),
                        paths.port_dir(action.spec),
                        build_package_options,
                        action.feature_list};
                    return Build::build_package(paths, build_config, status_db);
                }
                else
                {
                    const Build::BuildPackageConfig build_config{
                        action.any_paragraph.source_paragraph.value_or_exit(VCPKG_LINE_INFO),
                        action.spec.triplet(),
                        paths.port_dir(action.spec),
                        build_package_options};
                    return Build::build_package(paths, build_config, status_db);
                }
            }();

            if (result.code != Build::BuildResult::SUCCEEDED)
            {
                System::println(System::Color::error, Build::create_error_message(result.code, action.spec));
                return result.code;
            }

            System::println("Building package %s... done", display_name_with_features);

            const BinaryControlFile bcf =
                Paragraphs::try_load_cached_control_package(paths, action.spec).value_or_exit(VCPKG_LINE_INFO);
            System::println("Installing package %s... ", display_name_with_features);
            const auto install_result = install_package(paths, bcf, &status_db);
            switch (install_result)
            {
                case InstallResult::SUCCESS:
                    System::println(System::Color::success, "Installing package %s... done", display_name);
                    return BuildResult::SUCCEEDED;
                case InstallResult::FILE_CONFLICTS: return BuildResult::FILE_CONFLICTS;
                default: Checks::unreachable(VCPKG_LINE_INFO);
            }
        }

        if (plan_type == InstallPlanType::INSTALL)
        {
            if (use_head_version && is_user_requested)
            {
                System::println(
                    System::Color::warning, "Package %s is already built -- not building from HEAD", display_name);
            }
            System::println("Installing package %s... ", display_name);
            const auto install_result = install_package(
                paths, action.any_paragraph.binary_control_file.value_or_exit(VCPKG_LINE_INFO), &status_db);
            switch (install_result)
            {
                case InstallResult::SUCCESS:
                    System::println(System::Color::success, "Installing package %s... done", display_name);
                    return BuildResult::SUCCEEDED;
                case InstallResult::FILE_CONFLICTS: return BuildResult::FILE_CONFLICTS;
                default: Checks::unreachable(VCPKG_LINE_INFO);
            }
        }

        Checks::unreachable(VCPKG_LINE_INFO);
    }

    static void print_plan(const std::vector<AnyAction>& action_plan, bool is_recursive)
    {
        std::vector<const RemovePlanAction*> remove_plans;
        std::vector<const InstallPlanAction*> rebuilt_plans;
        std::vector<const InstallPlanAction*> only_install_plans;
        std::vector<const InstallPlanAction*> new_plans;
        std::vector<const InstallPlanAction*> already_installed_plans;

        const bool has_non_user_requested_packages = Util::find_if(action_plan, [](const AnyAction& package) -> bool {
                                                         if (auto iplan = package.install_plan.get())
                                                             return iplan->request_type != RequestType::USER_REQUESTED;
                                                         else
                                                             return false;
                                                     }) != action_plan.cend();

        for (auto&& action : action_plan)
        {
            if (auto install_action = action.install_plan.get())
            {
                // remove plans are guaranteed to come before install plans, so we know the plan will be contained if at
                // all.
                auto it = Util::find_if(
                    remove_plans, [&](const RemovePlanAction* plan) { return plan->spec == install_action->spec; });
                if (it != remove_plans.end())
                {
                    rebuilt_plans.emplace_back(install_action);
                }
                else
                {
                    switch (install_action->plan_type)
                    {
                        case InstallPlanType::INSTALL: only_install_plans.emplace_back(install_action); break;
                        case InstallPlanType::ALREADY_INSTALLED:
                            if (install_action->request_type == RequestType::USER_REQUESTED)
                                already_installed_plans.emplace_back(install_action);
                            break;
                        case InstallPlanType::BUILD_AND_INSTALL: new_plans.emplace_back(install_action); break;
                        default: Checks::unreachable(VCPKG_LINE_INFO);
                    }
                }
            }
            else if (auto remove_action = action.remove_plan.get())
            {
                remove_plans.emplace_back(remove_action);
            }
        }

        std::sort(remove_plans.begin(), remove_plans.end(), &RemovePlanAction::compare_by_name);
        std::sort(rebuilt_plans.begin(), rebuilt_plans.end(), &InstallPlanAction::compare_by_name);
        std::sort(only_install_plans.begin(), only_install_plans.end(), &InstallPlanAction::compare_by_name);
        std::sort(new_plans.begin(), new_plans.end(), &InstallPlanAction::compare_by_name);
        std::sort(already_installed_plans.begin(), already_installed_plans.end(), &InstallPlanAction::compare_by_name);

        if (already_installed_plans.size() > 0)
        {
            const std::string already_string =
                Strings::join("\n", already_installed_plans, [](const InstallPlanAction* p) {
                    return to_output_string(p->request_type, p->displayname());
                });
            System::println("The following packages are already installed:\n%s", already_string);
        }

        if (rebuilt_plans.size() > 0)
        {
            const std::string rebuilt_string = Strings::join("\n", rebuilt_plans, [](const InstallPlanAction* p) {
                return to_output_string(p->request_type, p->displayname());
            });
            System::println("The following packages will be rebuilt:\n%s", rebuilt_string);
        }

        if (new_plans.size() > 0)
        {
            const std::string new_string = Strings::join("\n", new_plans, [](const InstallPlanAction* p) {
                return to_output_string(p->request_type, p->displayname());
            });
            System::println("The following packages will be built and installed:\n%s", new_string);
        }

        if (only_install_plans.size() > 0)
        {
            const std::string only_install_string =
                Strings::join("\n", only_install_plans, [](const InstallPlanAction* p) {
                    return to_output_string(p->request_type, p->displayname());
                });
            System::println("The following packages will be directly installed:\n%s", only_install_string);
        }

        if (has_non_user_requested_packages)
            System::println("Additional packages (*) will be installed to complete this operation.");

        if (remove_plans.size() > 0 && !is_recursive)
        {
            System::println(System::Color::warning,
                            "If you are sure you want to rebuild the above packages, run the command with the "
                            "--recurse option");
            Checks::exit_fail(VCPKG_LINE_INFO);
        }
    }

    void perform_and_exit_ex(const std::vector<AnyAction>& action_plan,
                             const Build::BuildPackageOptions& install_plan_options,
                             const KeepGoing keep_going,
                             const PrintSummary print_summary,
                             const VcpkgPaths& paths,
                             StatusParagraphs& status_db)
    {
        std::vector<BuildResult> results;
        std::vector<std::string> timing;
        const auto timer = Chrono::ElapsedTime::create_started();
        size_t counter = 0;
        const size_t package_count = action_plan.size();

        for (const auto& action : action_plan)
        {
            const auto build_timer = Chrono::ElapsedTime::create_started();
            counter++;

            const std::string display_name = action.spec().to_string();
            System::println("Starting package %d/%d: %s", counter, package_count, display_name);

            timing.push_back("0");
            results.push_back(BuildResult::NULLVALUE);

            if (const auto install_action = action.install_plan.get())
            {
                const BuildResult result =
                    perform_install_plan_action(paths, *install_action, install_plan_options, status_db);
                if (result != BuildResult::SUCCEEDED && keep_going == KeepGoing::NO)
                {
                    System::println(Build::create_user_troubleshooting_message(install_action->spec));
                    Checks::exit_fail(VCPKG_LINE_INFO);
                }

                results.back() = result;
            }
            else if (const auto remove_action = action.remove_plan.get())
            {
                Checks::check_exit(VCPKG_LINE_INFO, GlobalState::feature_packages);
                Remove::perform_remove_plan_action(paths, *remove_action, Remove::Purge::YES, status_db);
            }
            else
            {
                Checks::unreachable(VCPKG_LINE_INFO);
            }

            timing.back() = build_timer.to_string();
            System::println("Elapsed time for package %s: %s", display_name, build_timer.to_string());
        }

        System::println("Total time taken: %s", timer.to_string());

        if (print_summary == PrintSummary::YES)
        {
            for (size_t i = 0; i < results.size(); i++)
            {
                System::println("%s: %s: %s", action_plan[i].spec(), Build::to_string(results[i]), timing[i]);
            }

            std::map<BuildResult, int> summary;
            for (const BuildResult& v : Build::BUILD_RESULT_VALUES)
            {
                summary[v] = 0;
            }

            for (const BuildResult& r : results)
            {
                summary[r]++;
            }

            System::println("\n\nSUMMARY");
            for (const std::pair<const BuildResult, int>& entry : summary)
            {
                System::println("    %s: %d", Build::to_string(entry.first), entry.second);
            }
        }

        Checks::exit_success(VCPKG_LINE_INFO);
    }

    static const std::string OPTION_DRY_RUN = "--dry-run";
    static const std::string OPTION_USE_HEAD_VERSION = "--head";
    static const std::string OPTION_NO_DOWNLOADS = "--no-downloads";
    static const std::string OPTION_RECURSE = "--recurse";
    static const std::string OPTION_KEEP_GOING = "--keep-going";

    static const std::array<std::string, 5> INSTALL_SWITCHES = {
        OPTION_DRY_RUN,
        OPTION_USE_HEAD_VERSION,
        OPTION_NO_DOWNLOADS,
        OPTION_RECURSE,
        OPTION_KEEP_GOING,
    };
    static const std::array<std::string, 0> INSTALL_SETTINGS;

    static std::vector<std::string> valid_arguments(const VcpkgPaths& paths)
    {
        auto sources_and_errors = Paragraphs::try_load_all_ports(paths.get_filesystem(), paths.ports);

        return Util::fmap(sources_and_errors.paragraphs,
                          [](auto&& pgh) -> std::string { return pgh->core_paragraph->name; });
    }

    const CommandStructure COMMAND_STRUCTURE = {
        "install zlib zlib:x64-windows curl boost",
        1,
        SIZE_MAX,
        INSTALL_SWITCHES,
        INSTALL_SETTINGS,
        &valid_arguments,
    };

    void perform_and_exit(const VcpkgCmdArguments& args, const VcpkgPaths& paths, const Triplet& default_triplet)
    {
        // input sanitization
        static const std::string EXAMPLE = Help::create_example_string("install zlib zlib:x64-windows curl boost");
        args.check_min_arg_count(1, EXAMPLE);

        const std::vector<FullPackageSpec> specs = Util::fmap(args.command_arguments, [&](auto&& arg) {
            return Input::check_and_get_full_package_spec(arg, default_triplet, EXAMPLE);
        });

        for (auto&& spec : specs)
        {
            Input::check_triplet(spec.package_spec.triplet(), paths);
            if (!spec.features.empty() && !GlobalState::feature_packages)
            {
                Checks::exit_with_message(
                    VCPKG_LINE_INFO, "Feature packages are experimentally available under the --featurepackages flag.");
            }
        }

        const std::unordered_set<std::string> options = args.check_and_get_optional_command_arguments(
            {OPTION_DRY_RUN, OPTION_USE_HEAD_VERSION, OPTION_NO_DOWNLOADS, OPTION_RECURSE, OPTION_KEEP_GOING});
        const bool dry_run = options.find(OPTION_DRY_RUN) != options.cend();
        const bool use_head_version = options.find(OPTION_USE_HEAD_VERSION) != options.cend();
        const bool no_downloads = options.find(OPTION_NO_DOWNLOADS) != options.cend();
        const bool is_recursive = options.find(OPTION_RECURSE) != options.cend();
        const KeepGoing keep_going = to_keep_going(options.find(OPTION_KEEP_GOING) != options.cend());
        const PrintSummary print_summary = to_print_summary(keep_going == KeepGoing::YES);

        // create the plan
        StatusParagraphs status_db = database_load_check(paths);

        const Build::BuildPackageOptions install_plan_options = {Build::to_use_head_version(use_head_version),
                                                                 Build::to_allow_downloads(!no_downloads)};

        std::vector<AnyAction> action_plan;

        if (GlobalState::feature_packages)
        {
            std::unordered_map<std::string, SourceControlFile> scf_map;
            auto all_ports = Paragraphs::load_all_ports(paths.get_filesystem(), paths.ports);
            for (auto&& port : all_ports)
            {
                scf_map[port->core_paragraph->name] = std::move(*port);
            }
            action_plan = create_feature_install_plan(scf_map, FullPackageSpec::to_feature_specs(specs), status_db);
        }
        else
        {
            Dependencies::PathsPortFile paths_port_file(paths);
            auto install_plan = Dependencies::create_install_plan(
                paths_port_file, Util::fmap(specs, [](auto&& spec) { return spec.package_spec; }), status_db);

            action_plan = Util::fmap(
                install_plan, [](InstallPlanAction& install_action) { return AnyAction(std::move(install_action)); });
        }

        // install plan will be empty if it is already installed - need to change this at status paragraph part
        Checks::check_exit(VCPKG_LINE_INFO, !action_plan.empty(), "Install plan cannot be empty");

        // log the plan
        const std::string specs_string = Strings::join(",", action_plan, [](const AnyAction& action) {
            if (auto iaction = action.install_plan.get())
                return iaction->spec.to_string();
            else if (auto raction = action.remove_plan.get())
                return "R$" + raction->spec.to_string();
            Checks::unreachable(VCPKG_LINE_INFO);
        });

        Metrics::g_metrics.lock()->track_property("installplan", specs_string);

        print_plan(action_plan, is_recursive);

        if (dry_run)
        {
            Checks::exit_success(VCPKG_LINE_INFO);
        }

        perform_and_exit_ex(action_plan, install_plan_options, keep_going, print_summary, paths, status_db);

        Checks::exit_success(VCPKG_LINE_INFO);
    }
}
