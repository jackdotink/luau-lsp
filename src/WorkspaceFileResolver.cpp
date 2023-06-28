#include <filesystem>
#include <optional>
#include <unordered_map>
#include <iostream>
#include "Luau/Ast.h"
#include "LSP/WorkspaceFileResolver.hpp"
#include "LSP/Utils.hpp"

Luau::ModuleName WorkspaceFileResolver::getModuleName(const Uri& name)
{
    // Handle non-file schemes
    if (name.scheme != "file")
        return name.toString();

    auto fsPath = name.fsPath().generic_string();
    if (auto virtualPath = resolveToVirtualPath(fsPath))
    {
        return *virtualPath;
    }

    return fsPath;
}

const std::string WorkspaceFileResolver::normalisedUriString(const lsp::DocumentUri& uri) const
{
    auto uriString = uri.toString();

// As windows/macOS is case insensitive, we lowercase the URI string for simplicitly and to handle
// normalisation issues
#if defined(_WIN32) || defined(__APPLE__)
    uriString = toLower(uriString);
#endif

    return uriString;
}

const TextDocument* WorkspaceFileResolver::getTextDocument(const lsp::DocumentUri& uri) const
{
    auto it = managedFiles.find(normalisedUriString(uri));
    if (it != managedFiles.end())
        return &it->second;

    return nullptr;
}

const TextDocument* WorkspaceFileResolver::getTextDocumentFromModuleName(const Luau::ModuleName& name) const
{
    // Handle untitled: files
    if (Luau::startsWith(name, "untitled:"))
        return getTextDocument(Uri::parse(name));

    if (auto filePath = resolveToRealPath(name))
        return getTextDocument(Uri::file(*filePath));

    return nullptr;
}

TextDocumentPtr WorkspaceFileResolver::getOrCreateTextDocumentFromModuleName(const Luau::ModuleName& name)
{
    if (auto document = getTextDocumentFromModuleName(name))
        return {document, false};

    if (auto filePath = resolveToRealPath(name))
        if (auto source = readSource(name))
            return {new TextDocument{Uri::file(*filePath), "luau", 0, source->source}, true};


    return {nullptr, false};
}

std::optional<SourceNodePtr> WorkspaceFileResolver::getSourceNodeFromVirtualPath(const Luau::ModuleName& name) const
{
    if (virtualPathsToSourceNodes.find(name) == virtualPathsToSourceNodes.end())
        return std::nullopt;
    return virtualPathsToSourceNodes.at(name);
}

std::optional<SourceNodePtr> WorkspaceFileResolver::getSourceNodeFromRealPath(const std::string& name) const
{
    std::error_code ec;
    auto canonicalName = std::filesystem::weakly_canonical(name, ec);
    if (ec.value() != 0)
        canonicalName = name;
    auto strName = canonicalName.generic_string();
    if (realPathsToSourceNodes.find(strName) == realPathsToSourceNodes.end())
        return std::nullopt;
    return realPathsToSourceNodes.at(strName);
}

Luau::ModuleName WorkspaceFileResolver::getVirtualPathFromSourceNode(const SourceNodePtr& sourceNode) const
{
    return sourceNode->virtualPath;
}

std::optional<std::filesystem::path> WorkspaceFileResolver::getRealPathFromSourceNode(const SourceNodePtr& sourceNode) const
{
    // NOTE: this filepath is generated by the sourcemap, which is relative to the cwd where the sourcemap
    // command was run from. Hence, we concatenate it to the end of the workspace path
    // TODO: make sure this is correct once we make sourcemap.json generic
    auto filePath = sourceNode->getScriptFilePath();
    if (filePath)
        return rootUri.fsPath() / *filePath;
    return std::nullopt;
}

std::optional<Luau::ModuleName> WorkspaceFileResolver::resolveToVirtualPath(const std::string& name) const
{
    if (isVirtualPath(name))
    {
        return name;
    }
    else
    {
        auto sourceNode = getSourceNodeFromRealPath(name);
        if (!sourceNode)
            return std::nullopt;
        return getVirtualPathFromSourceNode(sourceNode.value());
    }
}

std::optional<std::filesystem::path> WorkspaceFileResolver::resolveToRealPath(const Luau::ModuleName& name) const
{
    if (isVirtualPath(name))
    {
        if (auto sourceNode = getSourceNodeFromVirtualPath(name))
        {
            return getRealPathFromSourceNode(*sourceNode);
        }
    }
    else
    {
        return name;
    }

    return std::nullopt;
}

std::optional<Luau::SourceCode> WorkspaceFileResolver::readSource(const Luau::ModuleName& name)
{
    Luau::SourceCode::Type sourceType = Luau::SourceCode::Module;
    std::optional<std::string> source;

    std::filesystem::path realFileName = name;
    if (isVirtualPath(name))
    {
        auto sourceNode = getSourceNodeFromVirtualPath(name);
        if (!sourceNode)
            return std::nullopt;
        auto filePath = getRealPathFromSourceNode(sourceNode.value());
        if (!filePath)
            return std::nullopt;
        realFileName = filePath.value();
        sourceType = sourceNode.value()->sourceCodeType();
    }
    else
    {
        sourceType = sourceCodeTypeFromPath(realFileName);
    }

    if (auto textDocument = getTextDocumentFromModuleName(name))
    {
        source = textDocument->getText();
    }
    else
    {
        source = readFile(realFileName);
        if (source && realFileName.extension() == ".json")
        {
            try
            {
                source = "--!strict\nreturn " + jsonValueToLuau(json::parse(*source));
            }
            catch (const std::exception& e)
            {
                // TODO: display diagnostic?
                std::cerr << "Failed to load JSON module: " << realFileName.generic_string() << " - " << e.what() << std::endl;
                return std::nullopt;
            }
        }
    }

    if (!source)
        return std::nullopt;

    return Luau::SourceCode{*source, sourceType};
}

/// Modify the context so that game/Players/LocalPlayer items point to the correct place
const std::string mapContext(const std::string& context)
{
    if (context == "game/Players/LocalPlayer/PlayerScripts")
        return "game/StarterPlayer/StarterPlayerScripts";
    else if (context == "game/Players/LocalPlayer/PlayerGui")
        return "game/StarterGui";
    else if (context == "game/Players/LocalPlayer/StarterGear")
        return "game/StarterPack";
    return context;
}

/// Returns the base path to use in a string require.
/// This depends on user configuration, whether requires are taken relative to file or workspace root, defaulting to the latter
std::filesystem::path WorkspaceFileResolver::getRequireBasePath(std::optional<Luau::ModuleName> fileModuleName) const
{
    if (!client)
        return rootUri.fsPath();

    auto config = client->getConfiguration(rootUri);
    switch (config.require.mode)
    {
    case RequireModeConfig::RelativeToWorkspaceRoot:
        return rootUri.fsPath();
    case RequireModeConfig::RelativeToFile:
    {
        if (fileModuleName.has_value())
        {
            auto filePath = resolveToRealPath(*fileModuleName);
            if (filePath)
                return filePath->parent_path();
            else
                return rootUri.fsPath();
        }
        else
        {
            return rootUri.fsPath();
        }
    }
    }

    return rootUri.fsPath();
}

// Resolve the string using a directory alias if present
std::optional<std::filesystem::path> resolveDirectoryAlias(
    const std::unordered_map<std::string, std::string>& directoryAliases, const std::string& str, bool includeExtension)
{
    for (const auto& [alias, path] : directoryAliases)
    {
        if (Luau::startsWith(str, alias))
        {
            std::filesystem::path directoryPath = path;
            std::string remainder = str.substr(alias.length());

            auto filePath = resolvePath(remainder.empty() ? directoryPath : directoryPath / remainder);
            if (includeExtension && !filePath.has_extension())
            {
                if (std::filesystem::exists(filePath.replace_extension(".luau")))
                    return filePath.replace_extension(".luau");
                else
                    return filePath.replace_extension(".lua");
            }
            return filePath;
        }
    }

    return std::nullopt;
}

std::optional<Luau::ModuleInfo> WorkspaceFileResolver::resolveModule(const Luau::ModuleInfo* context, Luau::AstExpr* node)
{
    // Handle require("path") for compatibility
    if (auto* expr = node->as<Luau::AstExprConstantString>())
    {
        std::string requiredString(expr->value.data, expr->value.size);

        std::filesystem::path basePath = getRequireBasePath(context ? std::optional(context->name) : std::nullopt);
        auto filePath = basePath / requiredString;

        // Check for custom require overrides
        if (client)
        {
            auto config = client->getConfiguration(rootUri);

            // Check file aliases
            if (auto it = config.require.fileAliases.find(requiredString); it != config.require.fileAliases.end())
            {
                filePath = resolvePath(it->second);
            }
            // Check directory aliases
            else if (auto aliasedPath = resolveDirectoryAlias(config.require.directoryAliases, requiredString))
            {
                filePath = aliasedPath.value();
            }
        }

        std::error_code ec;

        // Handle "init.luau" files in a directory
        if (std::filesystem::is_directory(filePath, ec))
        {
            filePath /= "init.luau";
        }

        // Add file endings
        auto fullFilePath = std::filesystem::weakly_canonical(filePath.replace_extension(".luau"), ec);
        if (ec.value() != 0 || !std::filesystem::exists(filePath))
        {
            // fall back to .lua if a module with .luau doesn't exist
            filePath = std::filesystem::weakly_canonical(filePath.replace_extension(".lua"), ec);
        }

        // URI-ify the file path so that its normalised (in particular, the drive letter)
        return {{Uri::parse(Uri::file(filePath).toString()).fsPath().generic_string()}};
    }
    else if (auto* g = node->as<Luau::AstExprGlobal>())
    {
        if (g->name == "game")
            return Luau::ModuleInfo{"game"};

        if (g->name == "script")
        {
            if (auto virtualPath = resolveToVirtualPath(context->name))
            {
                return Luau::ModuleInfo{virtualPath.value()};
            }
        }
    }
    else if (auto* i = node->as<Luau::AstExprIndexName>())
    {
        if (context)
        {
            if (strcmp(i->index.value, "Parent") == 0)
            {
                // Pop the name instead
                auto parentPath = getParentPath(context->name);
                if (parentPath.has_value())
                    return Luau::ModuleInfo{parentPath.value(), context->optional};
            }

            return Luau::ModuleInfo{mapContext(context->name) + '/' + i->index.value, context->optional};
        }
    }
    else if (auto* i_expr = node->as<Luau::AstExprIndexExpr>())
    {
        if (auto* index = i_expr->index->as<Luau::AstExprConstantString>())
        {
            if (context)
                return Luau::ModuleInfo{mapContext(context->name) + '/' + std::string(index->value.data, index->value.size), context->optional};
        }
    }
    else if (auto* call = node->as<Luau::AstExprCall>(); call && call->self && call->args.size >= 1 && context)
    {
        if (auto* index = call->args.data[0]->as<Luau::AstExprConstantString>())
        {
            Luau::AstName func = call->func->as<Luau::AstExprIndexName>()->index;

            if (func == "GetService" && context->name == "game")
            {
                return Luau::ModuleInfo{"game/" + std::string(index->value.data, index->value.size)};
            }
            else if (func == "WaitForChild" || (func == "FindFirstChild" && call->args.size == 1)) // Don't allow recursive FFC
            {
                if (context)
                    return Luau::ModuleInfo{mapContext(context->name) + '/' + std::string(index->value.data, index->value.size), context->optional};
            }
            else if (func == "FindFirstAncestor")
            {
                if (context)
                {
                    auto ancestorName = getAncestorPath(context->name, std::string(index->value.data, index->value.size));
                    if (ancestorName)
                        return Luau::ModuleInfo{*ancestorName, context->optional};
                }
            }
        }
    }

    return std::nullopt;
}

std::string WorkspaceFileResolver::getHumanReadableModuleName(const Luau::ModuleName& name) const
{
    if (isVirtualPath(name))
    {
        if (auto realPath = resolveToRealPath(name))
        {
            return realPath->relative_path().generic_string() + " [" + name + "]";
        }
        else
        {
            return name;
        }
    }
    else
    {
        return name;
    }
}

const Luau::Config& WorkspaceFileResolver::getConfig(const Luau::ModuleName& name) const
{
    std::optional<std::filesystem::path> realPath = resolveToRealPath(name);
    if (!realPath || !realPath->has_relative_path() || !realPath->has_parent_path())
        return defaultConfig;

    return readConfigRec(realPath->parent_path());
}

const Luau::Config& WorkspaceFileResolver::readConfigRec(const std::filesystem::path& path) const
{
    auto it = configCache.find(path.generic_string());
    if (it != configCache.end())
        return it->second;

    Luau::Config result = (path.has_relative_path() && path.has_parent_path()) ? readConfigRec(path.parent_path()) : defaultConfig;
    auto configPath = path / Luau::kConfigName;

    if (std::optional<std::string> contents = readFile(configPath))
    {
        auto configUri = Uri::file(configPath);
        std::optional<std::string> error = Luau::parseConfig(*contents, result);
        if (error)
        {
            if (client)
            {
                lsp::Diagnostic diagnostic{{{0, 0}, {0, 0}}};
                diagnostic.message = *error;
                diagnostic.severity = lsp::DiagnosticSeverity::Error;
                diagnostic.source = "Luau";
                client->publishDiagnostics({configUri, std::nullopt, {diagnostic}});
            }
            else
                configErrors.emplace_back(std::pair{configPath, *error});
        }
        else
        {
            if (client)
                // Clear errors presented for file
                client->publishDiagnostics({configUri, std::nullopt, {}});
        }
    }

    return configCache[path.generic_string()] = result;
}

void WorkspaceFileResolver::clearConfigCache()
{
    configCache.clear();
    configErrors.clear();
}

void WorkspaceFileResolver::writePathsToMap(const SourceNodePtr& node, const std::string& base)
{
    node->virtualPath = base;
    virtualPathsToSourceNodes[base] = node;

    if (auto realPath = node->getScriptFilePath())
    {
        std::error_code ec;
        auto canonicalName = std::filesystem::weakly_canonical(rootUri.fsPath() / *realPath, ec);
        if (ec.value() != 0)
            canonicalName = *realPath;
        realPathsToSourceNodes[canonicalName.generic_string()] = node;
    }

    for (auto& child : node->children)
    {
        child->parent = node;
        writePathsToMap(child, base + "/" + child->name);
    }
}

void WorkspaceFileResolver::updateSourceMap(const std::string& sourceMapContents)
{
    realPathsToSourceNodes.clear();
    virtualPathsToSourceNodes.clear();

    try
    {
        auto j = json::parse(sourceMapContents);
        rootSourceNode = std::make_shared<SourceNode>(j.get<SourceNode>());

        // Mutate with plugin info
        if (pluginInfo)
        {
            if (rootSourceNode->className == "DataModel")
            {
                rootSourceNode->mutateWithPluginInfo(pluginInfo);
            }
            else
            {
                std::cerr << "Attempted to update plugin information for a non-DM instance" << std::endl;
            }
        }

        // Write paths
        std::string base = rootSourceNode->className == "DataModel" ? "game" : "ProjectRoot";
        writePathsToMap(rootSourceNode, base);
    }
    catch (const std::exception& e)
    {
        // TODO: log message?
        std::cerr << e.what() << std::endl;
    }
}
