#include <s2e/Plugin.h>

// XXX: hack: for now we include and register all plugins right there
#include <s2e/CorePlugin.h>
#include <s2e/Plugins/Example.h>

#include <algorithm>
#include <assert.h>

namespace s2e {

using namespace std;

void Plugin::initialize()
{
}

PluginsFactory::PluginsFactory()
{
#define __S2E_REGISTER_PLUGIN(className) \
    registerPlugin(className::getPluginInfoStatic())

    __S2E_REGISTER_PLUGIN(CorePlugin);
    __S2E_REGISTER_PLUGIN(plugins::Example);

#undef __S2E_REGISTER_PLUGIN
}

void PluginsFactory::registerPlugin(const PluginInfo* pluginInfo)
{
    assert(m_pluginsMap.find(pluginInfo->name) == m_pluginsMap.end());
    //assert(find(pluginInfo, m_pluginsList.begin(), m_pluginsList.end()) ==
      //                                              m_pluginsList.end());

    m_pluginsList.push_back(pluginInfo);
    m_pluginsMap.insert(make_pair(pluginInfo->name, pluginInfo));
}

const vector<const PluginInfo*>& PluginsFactory::getPluginInfoList() const
{
    return m_pluginsList;
}

const PluginInfo* PluginsFactory::getPluginInfo(const string& name) const
{
    map<string, const PluginInfo*>::const_iterator it =
                                m_pluginsMap.find(name);

    if(it != m_pluginsMap.end())
        return it->second;
    else
        return NULL;
}

Plugin* PluginsFactory::createPlugin(S2E* s2e, const string& name) const
{
    const PluginInfo* pluginInfo = getPluginInfo(name);
    if(pluginInfo)
        return pluginInfo->instanceCreator(s2e);
    else
        return NULL;
}

} // namespace s2e