// The Prepare tab's views (app.md §4.2, W8b), over PrepareModel: the Mac shell's ShellPrepareTab
// layout (PrepareView.swift's loadView): the playlists' outline on the left (300 DIP); on the right
// the playlist's name, the summary and its note, "Keep the stems until", one row of buttons (Check
// Again and Prepare, or while a run goes its bar, Pause and Stop in their place) and the tracks.
//  - The outline is a virtualized TreeView (recycling, ScrollUnit Item), the tracks a virtualized
//    ListView (recycling): rows are the model's keyed rows, updated in place.
//  - User intent comes only from input: the TreeViewItems' IsSelected and IsExpanded are bound
//    TwoWay to the rows, and only a target-to-source update (a click, a key, the expander) reaches
//    the model's setters; no Expanded/Collapsed/Selected event is handled (recycling re-binds them
//    during layout). The date picker's change is the user's unless the render is setting it.
//  - Every text is the core's (texts.def) or the snapshot's.
using System;
using System.ComponentModel;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Data;
using System.Windows.Documents;
using System.Windows.Markup;
using System.Windows.Media;
using RbStemsPlus.Core;
using CoreText = RbStemsPlus.Core.Text;

namespace RbStemsPlus.Shell
{
    internal sealed class PrepareView : Grid
    {
        public readonly PrepareModel Model;
        public readonly TreeView Outline = new TreeView();
        public readonly TextBlock NameLabel = new TextBlock { FontWeight = FontWeights.SemiBold, FontSize = 16, TextTrimming = TextTrimming.CharacterEllipsis };
        public readonly TextBlock Info = new TextBlock { TextWrapping = TextWrapping.Wrap, MinHeight = 48 };
        public readonly DatePicker Until = new DatePicker { MinWidth = 140, SelectedDateFormat = DatePickerFormat.Short };
        public readonly Button PrepareBtn = new Button(), CheckBtn = new Button(), PauseBtn = new Button(), StopBtn = new Button();
        public readonly ProgressBar Bar = new ProgressBar { Minimum = 0, Maximum = 1, Height = 16, MinWidth = 120, Margin = new Thickness(8, 0, 8, 0) };
        public readonly ListView Tracks = new ListView();
        private readonly UntilPicker untilPicker;
        private int pendingTrack = -1;
        private bool pendingOutline;
        private bool pendingFocus;

        public PrepareView(PrepareModel model)
        {
            Model = model;
            DataContext = model;
            Margin = new Thickness(16, 12, 16, 12);
            ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(300) });
            ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(16) });
            ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            BuildOutline();
            Children.Add(Outline);
            var right = BuildRight();
            SetColumn(right, 2);
            Children.Add(right);
            model.PropertyChanged += Changed;
            model.ScrollTracksTo += r => { pendingTrack = r; ApplyOneShots(); };
            model.ScrollOutlineTo += row => { pendingOutline = true; ApplyOneShots(); };
            untilPicker = new UntilPicker(model, () => Until.SelectedDate, d => Until.SelectedDate = d,
                (from, to) => { Until.DisplayDateStart = from; Until.DisplayDateEnd = to; }, a => Dispatcher.BeginInvoke(a));
            model.Applied += DrawDate;   // a refused day drawn back, the range after midnight (PT1)
            model.FocusOutline += () => { pendingFocus = true; ApplyOneShots(); };
            Loaded += (s, e) => { DrawAll(); ApplyOneShots(); };
            IsVisibleChanged += (s, e) => ApplyOneShots();
            DrawAll();
        }

        // ------------------------------------------------------------------ layout

        private void BuildOutline()
        {
            AutomationProperties.SetName(Outline, Texts.Get(CoreText.T_PREP_RB_PLAYLISTS));
            VirtualizingPanel.SetIsVirtualizing(Outline, true);
            VirtualizingPanel.SetVirtualizationMode(Outline, VirtualizationMode.Recycling);
            VirtualizingPanel.SetScrollUnit(Outline, ScrollUnit.Item);
            ScrollViewer.SetCanContentScroll(Outline, true);
            Outline.ItemsSource = Model.Roots;
            // the row: the kind's glyph (Segoe Fluent Icons: a folder, a list, a gear for an
            // intelligent playlist) and the title, secondary when it can't be prepared
            Outline.ItemTemplate = (DataTemplate)XamlReader.Parse(@"
<HierarchicalDataTemplate xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation'
                          xmlns:x='http://schemas.microsoft.com/winfx/2006/xaml' ItemsSource='{Binding Children}'>
  <StackPanel Orientation='Horizontal'>
    <TextBlock x:Name='Glyph' FontFamily='Segoe Fluent Icons, Segoe MDL2 Assets' Margin='0,0,6,0' VerticalAlignment='Center'
               Text='&#xE8FD;' />
    <TextBlock x:Name='Title' Text='{Binding Title}' VerticalAlignment='Center' />
  </StackPanel>
  <HierarchicalDataTemplate.Triggers>
    <DataTrigger Binding='{Binding Kind}' Value='0'><Setter TargetName='Glyph' Property='Text' Value='&#xE8B7;' /></DataTrigger>
    <DataTrigger Binding='{Binding Kind}' Value='2'><Setter TargetName='Glyph' Property='Text' Value='&#xE713;' /></DataTrigger>
    <DataTrigger Binding='{Binding Secondary}' Value='True'>
      <Setter TargetName='Title' Property='Foreground' Value='{DynamicResource {x:Static SystemColors.GrayTextBrushKey}}' />
      <Setter TargetName='Glyph' Property='Foreground' Value='{DynamicResource {x:Static SystemColors.GrayTextBrushKey}}' />
    </DataTrigger>
  </HierarchicalDataTemplate.Triggers>
</HierarchicalDataTemplate>");
            // intent only from input: TwoWay bindings, no container events (app.md §4.2)
            Outline.ItemContainerStyle = (Style)XamlReader.Parse(@"
<Style xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation' TargetType='TreeViewItem'>
  <Setter Property='IsExpanded' Value='{Binding IsExpanded, Mode=TwoWay}' />
  <Setter Property='IsSelected' Value='{Binding IsSelected, Mode=TwoWay}' />
  <Setter Property='AutomationProperties.Name' Value='{Binding Title}' />
</Style>");
            Outline.SetBinding(IsEnabledProperty, new Binding(nameof(PrepareModel.OutlineEnabled)));
        }

        private UIElement BuildRight()
        {
            var right = new DockPanel { LastChildFill = true };
            var top = new StackPanel();
            top.Children.Add(NameLabel);
            Info.Margin = new Thickness(0, 6, 0, 10);
            AutomationProperties.SetLiveSetting(Info, AutomationLiveSetting.Polite);
            top.Children.Add(Info);
            var keep = new StackPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(0, 0, 0, 10) };
            var keepLabel = new TextBlock { Text = Texts.Get(CoreText.T_PREP_KEEP_UNTIL), VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(0, 0, 8, 0) };
            keep.Children.Add(keepLabel);
            keep.Children.Add(Until);
            AutomationProperties.SetName(Until, keepLabel.Text);
            Until.SetBinding(IsEnabledProperty, new Binding(nameof(PrepareModel.UntilEnabled)));
            Until.SelectedDateChanged += (s, e) => untilPicker.Changed();   // the user's day, once (UntilPicker)
            Until.CalendarOpened += (s, e) => DrawDate();   // the range from today, even with no render since midnight
            top.Children.Add(keep);
            // one row: Check Again and Prepare, or the run's bar, Pause and Stop in their place. The
            // children go in the visual order (the keyboard's Tab order, app.md §4.7): Check Again
            // and the bar on the left, then Pause, Stop and Prepare on the right
            var buttons = new DockPanel { Margin = new Thickness(0, 0, 0, 10), LastChildFill = false };
            Wire(CheckBtn, CoreText.T_BTN_CHECK_AGAIN, "check_again", nameof(PrepareModel.CheckEnabled), nameof(PrepareModel.NotRunning));
            DockPanel.SetDock(CheckBtn, Dock.Left);
            buttons.Children.Add(CheckBtn);
            // RangeBase.Value binds TwoWay by default, and Progress has no setter: OneWay
            Bar.SetBinding(RangeBase.ValueProperty, new Binding(nameof(PrepareModel.Progress)) { Mode = BindingMode.OneWay });
            Bar.SetBinding(VisibilityProperty, new Binding(nameof(PrepareModel.Running)) { Converter = new BooleanToVisibilityConverter() });
            DockPanel.SetDock(Bar, Dock.Left);
            buttons.Children.Add(Bar);
            var runButtons = new StackPanel { Orientation = Orientation.Horizontal };
            Wire(PauseBtn, CoreText.T_BTN_PAUSE, "pause", null, nameof(PrepareModel.Running));
            runButtons.Children.Add(PauseBtn);
            Wire(StopBtn, CoreText.T_BTN_STOP, "stop", null, nameof(PrepareModel.Running));
            runButtons.Children.Add(StopBtn);
            Wire(PrepareBtn, CoreText.T_BTN_PREPARE, "prepare", nameof(PrepareModel.PrepareEnabled), nameof(PrepareModel.NotRunning));
            PrepareBtn.IsDefault = true;   // Return, as the Mac's keyEquivalent "\r"
            runButtons.Children.Add(PrepareBtn);
            DockPanel.SetDock(runButtons, Dock.Right);
            buttons.Children.Add(runButtons);
            top.Children.Add(buttons);
            DockPanel.SetDock(top, Dock.Top);
            right.Children.Add(top);
            // the tracks: Track, Result, Note (secondary)
            VirtualizingPanel.SetIsVirtualizing(Tracks, true);
            VirtualizingPanel.SetVirtualizationMode(Tracks, VirtualizationMode.Recycling);
            Tracks.ItemsSource = Model.Tracks;
            Tracks.ItemContainerStyle = SpokenRows(typeof(ListViewItem));
            var grid = new GridView();
            grid.Columns.Add(new GridViewColumn { Header = Texts.Get(CoreText.T_COL_TRACK), Width = 240, DisplayMemberBinding = new Binding(nameof(TrackRow.Name)) });
            grid.Columns.Add(new GridViewColumn { Header = Texts.Get(CoreText.T_COL_RESULT), Width = 100, DisplayMemberBinding = new Binding(nameof(TrackRow.Result)) });
            grid.Columns.Add(new GridViewColumn
            {
                Header = Texts.Get(CoreText.T_COL_NOTE),
                Width = 280,
                CellTemplate = (DataTemplate)XamlReader.Parse(@"
<DataTemplate xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation' xmlns:x='http://schemas.microsoft.com/winfx/2006/xaml'>
  <TextBlock Text='{Binding Why}'>
    <TextBlock.Style><Style TargetType='TextBlock'>
      <Setter Property='Foreground' Value='{DynamicResource {x:Static SystemColors.GrayTextBrushKey}}' />
      <Style.Triggers>
        <DataTrigger Binding='{Binding IsSelected, RelativeSource={RelativeSource AncestorType=ListViewItem}}' Value='True'>
          <Setter Property='Foreground' Value='{Binding (TextElement.Foreground), RelativeSource={RelativeSource AncestorType=ContentPresenter}}' />
        </DataTrigger>
      </Style.Triggers>
    </Style></TextBlock.Style>
  </TextBlock>
</DataTemplate>"),
            });
            Tracks.View = grid;
            right.Children.Add(Tracks);
            return right;
        }

        /// The rows' Narrator names from their cells (TrackRow.Spoken, SetRow.Spoken), never the
        /// row type's name (app.md §4.7); kept current by the binding as the cells change.
        internal static Style SpokenRows(Type container)
        {
            var style = new Style(container);
            style.Setters.Add(new Setter(AutomationProperties.NameProperty, new Binding("Spoken")));
            return style;
        }

        private void Wire(Button b, CoreText title, string id, string enabled, string visible)
        {
            b.Content = DialogWindow.NoAccessKey(Texts.Get(title));
            b.Padding = new Thickness(10, 3, 10, 3);
            b.Margin = new Thickness(4, 0, 0, 0);
            b.MinWidth = 80;
            b.Click += (s, e) => Model.Click(id);
            if (enabled != null) b.SetBinding(IsEnabledProperty, new Binding(enabled));
            b.SetBinding(VisibilityProperty, new Binding(visible) { Converter = new BooleanToVisibilityConverter() });
        }

        // ------------------------------------------------------------------ the model's values

        private void Changed(object sender, PropertyChangedEventArgs e)
        {
            switch (e.PropertyName)
            {
                case nameof(PrepareModel.Name): DrawName(); break;
                case nameof(PrepareModel.Info):
                case nameof(PrepareModel.InfoSub): DrawInfo(); break;
                case nameof(PrepareModel.PrepareTitle): DrawTitles(); break;
                case nameof(PrepareModel.PauseTitle): DrawTitles(); break;
                case nameof(PrepareModel.Until): DrawDate(); break;
            }
        }

        private void DrawAll()
        {
            DrawName();
            DrawInfo();
            DrawTitles();
            DrawDate();
        }

        // the name, "Choose a playlist" until the core names one (the Mac's label)
        private void DrawName() => NameLabel.Text = Model.Name.Length > 0 ? Model.Name : Texts.Get(CoreText.T_PREP_CHOOSE);

        // the summary in the text colour, its note after a line break in the secondary colour
        private void DrawInfo()
        {
            Info.Inlines.Clear();
            Info.Inlines.Add(new Run(Model.Info));
            if (Model.InfoSub.Length > 0)
            {
                Info.Inlines.Add(new LineBreak());
                var sub = new Run(Model.InfoSub);
                sub.SetResourceReference(TextElement.ForegroundProperty, SystemColors.GrayTextBrushKey);
                Info.Inlines.Add(sub);
            }
        }

        private void DrawTitles()
        {
            if (Model.PrepareTitle.Length > 0) PrepareBtn.Content = DialogWindow.NoAccessKey(Model.PrepareTitle);
            if (Model.PauseTitle.Length > 0) PauseBtn.Content = DialogWindow.NoAccessKey(Model.PauseTitle);
        }

        // the core's date, and the picker's range from today (tomorrow to a year on), as the core
        // checks it: set again when the day changed (a tab left open past midnight, SO3)
        private void DrawDate() => untilPicker?.Draw();

        // the one-shots, carried out once the tab is on screen (SO13)
        private void ApplyOneShots()
        {
            if (!IsLoaded || !IsVisible) return;
            if (pendingTrack >= 0 && pendingTrack < Model.Tracks.Count) Tracks.ScrollIntoView(Model.Tracks[pendingTrack]);
            pendingTrack = -1;
            // the core's selected row now (a full redraw may have replaced the rows since the
            // request), kept until it is brought into view
            if (pendingOutline && Model.OutlineAt(Model.SelectedRow) is OutlineRow row && BringIntoView(row)) pendingOutline = false;
            else if (pendingOutline && Model.OutlineAt(Model.SelectedRow) == null) pendingOutline = false;
            if (pendingFocus) Outline.Focus();
            pendingFocus = false;
        }

        // a row of the virtualized outline into view: each ancestor's container realized in turn;
        // false when a container couldn't be realized yet
        private bool BringIntoView(OutlineRow row)
        {
            var path = new System.Collections.Generic.List<OutlineRow>();
            for (OutlineRow r = row; r != null; r = Model.OutlineAt(r.Parent)) path.Insert(0, r);
            ItemsControl parent = Outline;
            foreach (var r in path)
            {
                parent.UpdateLayout();
                var item = parent.ItemContainerGenerator.ContainerFromItem(r) as TreeViewItem;
                if (item == null)
                {
                    int index = parent.Items.IndexOf(r);
                    if (index < 0) return false;
                    if (FindPanel(parent) is VirtualizingStackPanel panel) panel.BringIndexIntoViewPublic(index);
                    parent.UpdateLayout();
                    item = parent.ItemContainerGenerator.ContainerFromItem(r) as TreeViewItem;
                    if (item == null) return false;
                }
                if (r == row) { item.BringIntoView(); return true; }
                parent = item;
            }
            return false;
        }

        private static Panel FindPanel(DependencyObject d)
        {
            for (int i = 0; i < VisualTreeHelper.GetChildrenCount(d); i++)
            {
                var c = VisualTreeHelper.GetChild(d, i);
                if (c is VirtualizingStackPanel p) return p;
                var r = FindPanel(c);
                if (r != null) return r;
            }
            return null;
        }
    }
}
