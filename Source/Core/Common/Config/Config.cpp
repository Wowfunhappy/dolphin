// Copyright 2016 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include <algorithm>
#include <atomic>
#include <list>
#include <map>
#include <mutex>
#include <shared_mutex>

#include "Common/Config/Config.h"

namespace Config
{
    using Layers = std::map<LayerType, std::shared_ptr<Layer>>;
    static Layers s_layers;
    static std::list<ConfigChangedCallback> s_callbacks;
    static u32 s_callback_guards = 0;
    static std::atomic<u64> s_config_version = 0;
    
    Layers* GetLayers()
    {
        return &s_layers;
    }
    
    void AddLayer(std::unique_ptr<Layer> layer)
    {
        s_layers[layer->GetLayer()] = std::move(layer);
        OnConfigChanged();
    }
    
    void AddLayer(std::unique_ptr<ConfigLayerLoader> loader)
    {
        AddLayer(std::make_unique<Layer>(std::move(loader)));
    }
    
    std::shared_ptr<Layer> GetLayer(LayerType layer)
    {
        std::shared_ptr<Layer> result;
        const auto it = s_layers.find(layer);
        if (it != s_layers.end())
        {
            result = it->second;
        }
        return result;
    }
    
    void RemoveLayer(LayerType layer)
    {
        s_layers.erase(layer);
        OnConfigChanged();
    }
    bool LayerExists(LayerType layer)
    {
        return s_layers.find(layer) != s_layers.end();
    }
    
    void AddConfigChangedCallback(ConfigChangedCallback func)
    {
        s_callbacks.emplace_back(func);
    }
    
    void OnConfigChanged()
    {
        s_config_version.fetch_add(1, std::memory_order_relaxed);
        if (s_callback_guards)
            return;
        
        for (const auto& callback : s_callbacks)
            callback();
    }
    
    u64 GetConfigVersion()
    {
        return s_config_version.load(std::memory_order_relaxed);
    }
    
    // Explicit load and save of layers
    void Load()
    {
        for (auto& layer : s_layers)
            layer.second->Load();
        OnConfigChanged();
    }
    
    void Save()
    {
        for (auto& layer : s_layers)
            layer.second->Save();
        OnConfigChanged();
    }
    
    void Init()
    {
        // These layers contain temporary values
        ClearCurrentRunLayer();
    }
    
    void Shutdown()
    {
        s_layers.clear();
        s_callbacks.clear();
    }
    
    void ClearCurrentRunLayer()
    {
        s_layers[LayerType::CurrentRun] = std::make_unique<Layer>(LayerType::CurrentRun);
    }
    
    static const std::map<System, std::string> system_to_name = {
        {System::Main, "Dolphin"},          {System::GCPad, "GCPad"},    {System::WiiPad, "Wiimote"},
        {System::GCKeyboard, "GCKeyboard"}, {System::GFX, "Graphics"},   {System::Logger, "Logger"},
        {System::Debugger, "Debugger"},     {System::SYSCONF, "SYSCONF"}};
    
    const std::string& GetSystemName(System system)
    {
        return system_to_name.at(system);
    }
    
    std::optional<System> GetSystemFromName(const std::string& name)
    {
        const auto system = std::find_if(system_to_name.begin(), system_to_name.end(),
                                         [&name](const auto& entry) { return entry.second == name; });
        if (system != system_to_name.end())
            return system->first;
        
        return {};
    }
    
    const std::string& GetLayerName(LayerType layer)
    {
        static const std::map<LayerType, std::string> layer_to_name = {
            {LayerType::Base, "Base"},
            {LayerType::GlobalGame, "Global GameINI"},
            {LayerType::LocalGame, "Local GameINI"},
            {LayerType::Netplay, "Netplay"},
            {LayerType::Movie, "Movie"},
            {LayerType::CommandLine, "Command Line"},
            {LayerType::CurrentRun, "Current Run"},
        };
        return layer_to_name.at(layer);
    }
    
    LayerType GetActiveLayerForConfig(const Location& config)
    {
        for (auto layer : SEARCH_ORDER)
        {
            const auto it = s_layers.find(layer);
            if (it != s_layers.end())
            {
                if (it->second->Exists(config))
                    return layer;
            }
        }
        
        // If config is not present in any layer, base layer is considered active.
        return LayerType::Base;
    }
    
    std::optional<std::string> GetAsString(const Location& config)
    {
        std::optional<std::string> result;
        //ReadLock lock(s_layers_rw_lock);
        
        for (auto layer : SEARCH_ORDER)
        {
            const auto it = s_layers.find(layer);
            if (it != s_layers.end())
            {
                result = it->second->Get<std::string>(config);
                if (result.has_value())
                    break;
            }
        }
        
        return result;
    }
    
    ConfigChangeCallbackGuard::ConfigChangeCallbackGuard()
    {
        ++s_callback_guards;
    }
    
    ConfigChangeCallbackGuard::~ConfigChangeCallbackGuard()
    {
        if (--s_callback_guards)
            return;
        
        OnConfigChanged();
    }
    
}  // namespace Config
