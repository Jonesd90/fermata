#pragma once
#include "Uikit.h"

namespace td
{
/** A small editable table: text cells, drop-down cells and tick-box cells, driven by lambdas. */
class GridEditor : public juce::Component, private juce::TableListBoxModel
{
public:
    enum class Type { Text, ReadOnly, Combo, Toggle, Button };   // Button: get() = caption, set() is called on click

    struct Column
    {
        juce::String title;
        int width = 120;
        Type type = Type::Text;
        std::function<juce::String (int row)> get;
        std::function<void (int row, const juce::String& value)> set;
        juce::StringArray items;                              // for Type::Combo
        std::function<juce::StringArray (int row)> itemsFor;  // optional per-row items
    };

    GridEditor()
    {
        table.setModel (this);
        table.setRowHeight (26);
        table.setColour (juce::ListBox::backgroundColourId, theme::window);
        table.getHeader().setStretchToFitActive (false);
        addAndMakeVisible (table);
    }

    /** Called on right-click: add menu items (each with its own action) for the row. */
    std::function<void (int row, juce::PopupMenu&)> addContextItems;
    /** Called when the Delete / Backspace key is pressed with a row selected. */
    std::function<void (int row)> onDeleteKey;
    void selectRow (int r) { table.selectRow (r); }
    void setRowCountProvider (std::function<int()> f) { rowCount = std::move (f); }
    void addColumn (Column c)
    {
        cols.push_back (std::move (c));
        table.getHeader().addColumn (cols.back().title, (int) cols.size(), cols.back().width, 30, 600,
                                     juce::TableHeaderComponent::notResizableOrSortable);
    }
    void refresh() { table.updateContent(); table.repaint(); }
    int getSelectedRow() const { return table.getSelectedRow(); }
    void resized() override { table.setBounds (getLocalBounds()); }

private:
    class Cell : public juce::Component
    {
    public:
        explicit Cell (GridEditor& o) : owner (o)
        {
            addChildComponent (label); addChildComponent (combo); addChildComponent (toggle); addChildComponent (button);
            button.onClick = [this] { if (! updating) { owner.table.selectRow (row); commit ({}); } };
            label.setEditable (false, true, false);
            label.setColour (juce::Label::textColourId, theme::text);
            label.onTextChange = [this] { if (! updating) commit (label.getText()); };
            combo.onChange = [this] { if (! updating) commit (combo.getText()); };
            toggle.onClick = [this] { if (! updating) commit (toggle.getToggleState() ? "1" : "0"); };
            addMouseListener (this, true);       // also hear clicks on the child label / combo / tick box
        }
        void mouseDown (const juce::MouseEvent& e) override
        {
            owner.table.selectRow (row);
            if (e.mods.isPopupMenu() && owner.addContextItems)
            {
                juce::PopupMenu m;
                owner.addContextItems (row, m);
                if (m.getNumItems() > 0) m.showMenuAsync (juce::PopupMenu::Options());
            }
        }
        void update (int r, int c)
        {
            row = r; col = c;
            auto& cd = owner.cols[(size_t) c];
            updating = true;
            const auto v = cd.get ? cd.get (r) : juce::String();
            label.setVisible (cd.type == Type::Text || cd.type == Type::ReadOnly);
            combo.setVisible (cd.type == Type::Combo);
            toggle.setVisible (cd.type == Type::Toggle);
            button.setVisible (cd.type == Type::Button);
            label.setInterceptsMouseClicks (cd.type == Type::Text, false);        // a plain label must not swallow the click that selects the row
            label.setEditable (false, cd.type == Type::Text, false);
            if (cd.type == Type::Combo)
            {
                combo.clear (juce::dontSendNotification);
                combo.addItemList (cd.itemsFor ? cd.itemsFor (r) : cd.items, 1);
                combo.setText (v, juce::dontSendNotification);
            }
            else if (cd.type == Type::Toggle) toggle.setToggleState (v == "1", juce::dontSendNotification);
            else if (cd.type == Type::Button) button.setButtonText (v);
            else label.setText (v, juce::dontSendNotification);
            updating = false;
        }
        void resized() override { auto b = getLocalBounds().reduced (3, 2); label.setBounds (b); combo.setBounds (b); toggle.setBounds (b); button.setBounds (b.reduced (2, 0)); }
    private:
        void commit (const juce::String& v)
        {
            auto& cd = owner.cols[(size_t) col];
            if (cd.set) cd.set (row, v);
        }
        GridEditor& owner;
        juce::Label label; juce::ComboBox combo; juce::ToggleButton toggle; juce::TextButton button;
        int row = 0, col = 0; bool updating = false;
    };

    int getNumRows() override { return rowCount ? rowCount() : 0; }
    void paintRowBackground (juce::Graphics& g, int row, int, int, bool selected) override
    {
        g.fillAll (selected ? theme::selected : (row & 1) ? theme::rowAlt : theme::window);
    }
    void paintCell (juce::Graphics&, int, int, int, int, bool) override {}
    void deleteKeyPressed (int lastRowSelected) override { if (onDeleteKey && lastRowSelected >= 0) onDeleteKey (lastRowSelected); }
    juce::Component* refreshComponentForCell (int row, int columnId, bool, juce::Component* existing) override
    {
        auto* c = dynamic_cast<Cell*> (existing);
        if (c == nullptr) c = new Cell (*this);
        c->update (row, columnId - 1);
        return c;
    }

    juce::TableListBox table;
    std::vector<Column> cols;
    std::function<int()> rowCount;
};
} // namespace td
