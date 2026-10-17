#pragma once
#include <albert/extensionplugin.h>
#include <albert/globalqueryhandler.h>
#include <memory>
class Backend;

class Plugin : public albert::ExtensionPlugin,
               public albert::GlobalQueryHandler
{
    ALBERT_PLUGIN

public:

    Plugin();
    ~Plugin() override;

    QString defaultTrigger() const override;
    std::vector<albert::RankItem> rankItems(albert::QueryContext &) override;

private:

    // Shared with the actions of the items, which may outlive the plugin
    std::shared_ptr<Backend> backend_;

};
