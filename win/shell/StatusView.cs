// The Status tab's views (app.md §4.2, W8b), over StatusModel: the Mac shell's ShellStatusTab
// layout (StatusTab.loadView): the two features' boxes (each light, name and tip), "Prepared
// playlists" and its note, the prepared playlists' table, and one row with the run's line on the
// left and Stop Keeping… and Prepare Again on the right.
//  - The table is a ListView (recycling): rows are the model's keyed rows, updated in place.
//  - User intent comes only from input: the list's SelectedIndex is bound TwoWay to the model,
//    whose setter sends only a selection other than the one shown and never inside a render; a
//    double click on a row (not the header or the empty space) is Prepare Again's click, as the
//    Mac table's doubleAction.
//  - Every text is the core's (texts.def) or the snapshot's.
using System.ComponentModel;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using System.Windows.Data;
using System.Windows.Input;
using System.Windows.Markup;
using RbStemsPlus.Core;
using CoreText = RbStemsPlus.Core.Text;

namespace RbStemsPlus.Shell
{
    internal sealed class StatusView : DockPanel
    {
        public readonly StatusModel Model;
        private readonly ManageModel app;
        public readonly TextBlock PlusTip = Tip(), CacheTip = Tip();
        private readonly TextBlock plusLight = Light(), cacheLight = Light();
        private readonly TextBlock plusName = FeatureName(CoreText.T_FEATURE_PLUS), cacheName = FeatureName(CoreText.T_FEATURE_CACHE);
        public readonly ListView Table = new ListView { SelectionMode = SelectionMode.Single, MinHeight = 160 };
        public readonly Button AgainBtn = new Button(), StopBtn = new Button();
        public readonly TextBlock RunLine = new TextBlock { VerticalAlignment = VerticalAlignment.Center, TextTrimming = TextTrimming.CharacterEllipsis };
        public readonly TextBlock TitleLabel = new TextBlock { FontWeight = FontWeights.SemiBold, FontSize = 14, Margin = new Thickness(0, 0, 0, 4) };
        public readonly TextBlock NoteLabel = new TextBlock { TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 0, 0, 10) };

        public StatusView(StatusModel model, ManageModel app)
        {
            Model = model;
            this.app = app;
            DataContext = model;
            Margin = new Thickness(20, 16, 20, 16);
            LastChildFill = true;
            var features = new Grid { Margin = new Thickness(0, 0, 0, 14) };
            features.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            features.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(16) });
            features.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            var plusBox = Box(plusLight, plusName, PlusTip);
            var cacheBox = Box(cacheLight, cacheName, CacheTip);
            Grid.SetColumn(cacheBox, 2);
            features.Children.Add(plusBox);
            features.Children.Add(cacheBox);
            SetDock(features, Dock.Top);
            Children.Add(features);
            TitleLabel.Text = Texts.Get(CoreText.T_STATUS_TITLE);
            SetDock(TitleLabel, Dock.Top);
            Children.Add(TitleLabel);
            NoteLabel.Text = Texts.Get(CoreText.T_STATUS_NOTE);
            NoteLabel.SetResourceReference(TextBlock.ForegroundProperty, SystemColors.GrayTextBrushKey);
            SetDock(NoteLabel, Dock.Top);
            Children.Add(NoteLabel);
            // the buttons' row (Mac order: the run's line, a gap, Stop Keeping…, Prepare Again), the
            // buttons in the visual order (the keyboard's Tab order, app.md §4.7)
            var buttons = new DockPanel { Margin = new Thickness(0, 10, 0, 0), LastChildFill = true };
            var right = new StackPanel { Orientation = Orientation.Horizontal };
            Wire(StopBtn, CoreText.T_BTN_STOP_KEEPING, StatusModel.StopKeepingId, nameof(StatusModel.StopEnabled));
            right.Children.Add(StopBtn);
            Wire(AgainBtn, CoreText.T_BTN_PREPARE_AGAIN, StatusModel.PrepareAgainId, nameof(StatusModel.AgainEnabled));
            right.Children.Add(AgainBtn);
            SetDock(right, Dock.Right);
            buttons.Children.Add(right);
            RunLine.SetResourceReference(TextBlock.ForegroundProperty, SystemColors.GrayTextBrushKey);
            RunLine.SetBinding(TextBlock.TextProperty, new Binding(nameof(StatusModel.RunLine)));
            AutomationProperties.SetLiveSetting(RunLine, AutomationLiveSetting.Polite);
            buttons.Children.Add(RunLine);
            SetDock(buttons, Dock.Bottom);
            Children.Add(buttons);
            BuildTable();
            Children.Add(Table);   // last: LastChildFill
            // the keyboard's Tab order as shown (app.md §4.7): the table, Stop Keeping…, Prepare Again
            KeyboardNavigation.SetTabIndex(Table, 0);
            KeyboardNavigation.SetTabIndex(StopBtn, 1);
            KeyboardNavigation.SetTabIndex(AgainBtn, 2);
            app.PropertyChanged += AppChanged;
            SystemParameters.StaticPropertyChanged += (s, e) => { if (e.PropertyName == nameof(SystemParameters.HighContrast)) DrawLights(); };
            DrawLights();
        }

        // ------------------------------------------------------------------ layout

        private static TextBlock Tip()
        {
            var t = new TextBlock { TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 6, 0, 0) };
            t.SetResourceReference(TextBlock.ForegroundProperty, SystemColors.GrayTextBrushKey);
            return t;
        }

        // the light: Manage's "Light" style (Segoe Fluent Icons), drawn by MainWindow.Draw
        private static TextBlock Light() => new TextBlock
        {
            FontFamily = new System.Windows.Media.FontFamily("Segoe Fluent Icons, Segoe MDL2 Assets"),
            FontSize = 15,
            VerticalAlignment = VerticalAlignment.Center,
            Margin = new Thickness(0, 0, 6, 0),
        };

        private static TextBlock FeatureName(CoreText t) => new TextBlock { Text = Texts.Get(t), FontWeight = FontWeights.SemiBold, FontSize = 14, VerticalAlignment = VerticalAlignment.Center };

        // a feature's box: its light and name, its tip under them (the Mac's rounded box)
        private static Border Box(TextBlock light, TextBlock name, TextBlock tip)
        {
            var head = new StackPanel { Orientation = Orientation.Horizontal };
            head.Children.Add(light);
            head.Children.Add(name);
            var body = new StackPanel();
            body.Children.Add(head);
            body.Children.Add(tip);
            var b = new Border { CornerRadius = new CornerRadius(8), Padding = new Thickness(12), BorderThickness = new Thickness(1), Child = body };
            b.SetResourceReference(Border.BorderBrushProperty, SystemColors.ControlLightBrushKey);
            return b;
        }

        private void BuildTable()
        {
            AutomationProperties.SetName(Table, TitleLabel.Text);
            VirtualizingPanel.SetIsVirtualizing(Table, true);
            VirtualizingPanel.SetVirtualizationMode(Table, VirtualizationMode.Recycling);
            Table.ItemsSource = Model.Sets;
            Table.ItemContainerStyle = PrepareView.SpokenRows(typeof(ListViewItem));
            // intent only from input: the binding's target-to-source update is the user's (the
            // model ignores it inside a render, and a selection equal to the one shown)
            Table.SetBinding(System.Windows.Controls.Primitives.Selector.SelectedIndexProperty,
                new Binding(nameof(StatusModel.SelectedIndex)) { Mode = BindingMode.TwoWay });
            var grid = new GridView();
            grid.Columns.Add(new GridViewColumn { Header = Texts.Get(CoreText.T_COL_PLAYLIST), Width = 300, DisplayMemberBinding = new Binding(nameof(SetRow.Name)) });
            grid.Columns.Add(new GridViewColumn { Header = Texts.Get(CoreText.T_COL_TRACKS), Width = 70, DisplayMemberBinding = new Binding(nameof(SetRow.Tracks)) });
            grid.Columns.Add(new GridViewColumn { Header = Texts.Get(CoreText.T_COL_KEPT_UNTIL), Width = 130, DisplayMemberBinding = new Binding(nameof(SetRow.Until)) });
            grid.Columns.Add(new GridViewColumn
            {
                Header = Texts.Get(CoreText.T_COL_STATE),
                Width = 220,
                CellTemplate = (DataTemplate)XamlReader.Parse(@"
<DataTemplate xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation' xmlns:x='http://schemas.microsoft.com/winfx/2006/xaml'>
  <TextBlock Text='{Binding State}'>
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
            Table.View = grid;
            // a row's double click (never the header's or the empty space's): Prepare Again
            Table.MouseDoubleClick += (s, e) =>
            {
                if (e.ChangedButton != System.Windows.Input.MouseButton.Left) return;
                if (ItemsControl.ContainerFromElement(Table, e.OriginalSource as DependencyObject) is ListViewItem) Model.RowDoubleClicked();
            };
        }

        private void Wire(Button b, CoreText title, string id, string enabled)
        {
            b.Content = DialogWindow.NoAccessKey(Texts.Get(title));
            b.Padding = new Thickness(10, 3, 10, 3);
            b.Margin = new Thickness(8, 0, 0, 0);
            b.MinWidth = 80;
            b.IsEnabled = false;   // off until the first render gives its field (never a default on)
            b.Click += (s, e) => Model.Click(id);
            b.SetBinding(IsEnabledProperty, new Binding(enabled));
        }

        // ------------------------------------------------------------------ the lights (S_APP)

        private void AppChanged(object sender, PropertyChangedEventArgs e)
        {
            switch (e.PropertyName)
            {
                case nameof(ManageModel.PlusLook): case nameof(ManageModel.PlusAx): case nameof(ManageModel.PlusTip):
                case nameof(ManageModel.CacheLook): case nameof(ManageModel.CacheAx): case nameof(ManageModel.CacheTip):
                    DrawLights();
                    break;
            }
        }

        // each feature's light as Manage's, and its tip under the name (the Mac's setTips)
        private void DrawLights()
        {
            MainWindow.Draw(plusLight, plusName, app.PlusLook, app.PlusAx, app.PlusTip);
            MainWindow.Draw(cacheLight, cacheName, app.CacheLook, app.CacheAx, app.CacheTip);
            plusLight.Visibility = app.PlusLightShown ? Visibility.Visible : Visibility.Collapsed;
            cacheLight.Visibility = app.CacheLightShown ? Visibility.Visible : Visibility.Collapsed;
            if (PlusTip.Text != app.PlusTip) PlusTip.Text = app.PlusTip;
            if (CacheTip.Text != app.CacheTip) CacheTip.Text = app.CacheTip;
        }
    }
}
