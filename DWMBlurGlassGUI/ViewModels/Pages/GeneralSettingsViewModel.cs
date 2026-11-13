using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using DWMBlurGlassGUI.Models;
using DWMBlurGlassGUI.Services;
using DWMBlurGlassGUI.Views.Controls;
using DWMBlurGlassGUI.Views.Pages;
using Microsoft.Win32;
using System;
using System.Threading.Tasks;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using Wpf.Ui;
using Wpf.Ui.Controls;

namespace DWMBlurGlassGUI.ViewModels.Pages
{
    public partial class GeneralSettingsViewModel : ObservableObject
    {
        private readonly IContentDialogService _contentDialogService;
        private readonly INavigationService _navigationService;
        private readonly ConfigManager _configManager;
        private readonly SymbolDownloader _symbolDownloader;

        public LanguageService Lang => LanguageService.Instance;

        public GeneralSettingsViewModel(IContentDialogService contentDialogService, INavigationService navigationService, ConfigManager configManager)
        {
            _navigationService = navigationService;
            _contentDialogService = contentDialogService;
            _symbolDownloader = new SymbolDownloader();
            _configManager = configManager;
            _configManager.ConfigReloaded += (s, e) => LoadFromConfig();
            _configManager.Config.PropertyChanged += (s, e) =>
            {
                if (e.PropertyName == nameof(AppConfig.BlurMethod))
                    UpdateEnabledStates();
                if (e.PropertyName == nameof(AppConfig.CustomTitleBtnSize) ||
                    e.PropertyName == nameof(AppConfig.TitleBtnSizePreset))
                    OnPropertyChanged(nameof(TitleBtnSizePresetName));
            };
            _configManager.PropertyChanged += (s, e) =>
            {
                if (e.PropertyName == nameof(ConfigManager.IsModified))
                    SaveConfigCommand.NotifyCanExecuteChanged();
            };
            LoadWallpaper();
            LoadFromConfig();
            CheckInstallStatus();
        }

        private void CheckInstallStatus()
        {
            IsInstalled = TaskSchedulerService.IsTaskInstalled();
            StatusText = IsInstalled ? Lang["status1"] : Lang["status0"];
        }

        private void LoadFromConfig()
        {
            var config = _configManager.Config;
            OverrideDwmApi = config.ApplyGlobal;
            ExtendToBorder = config.ExtendBorder;
            EnableAeroReflection = config.Reflection;
            RestoreWin7ButtonSize = config.OldBtnHeight;
            EnableWin7ButtonGlow = config.TitleBtnGlow;
            CustomAmount = config.CustomAmount;
            BlurRadius = (int)config.BlurAmount;

            _lightActiveTitleBarColor = UIntToColor(config.ActiveBlendColor);
            _lightInactiveTitleBarColor = UIntToColor(config.InactiveBlendColor);
            _lightActiveTextColor = UIntToColor(config.ActiveTextColor);
            _lightInactiveTextColor = UIntToColor(config.InactiveTextColor);

            _darkActiveTitleBarColor = UIntToColor(config.ActiveBlendColorDark);
            _darkInactiveTitleBarColor = UIntToColor(config.InactiveBlendColorDark);
            _darkActiveTextColor = UIntToColor(config.ActiveTextColorDark);
            _darkInactiveTextColor = UIntToColor(config.InactiveTextColorDark);

            _lightActiveGradientColor = UIntToColor(config.ActiveGradientColor);
            _lightInactiveGradientColor = UIntToColor(config.InactiveGradientColor);
            _darkActiveGradientColor = UIntToColor(config.ActiveGradientColorDark);
            _darkInactiveGradientColor = UIntToColor(config.InactiveGradientColorDark);
            _lightActiveGradientMidColor = UIntToColor(config.ActiveGradientMidColor);
            _lightInactiveGradientMidColor = UIntToColor(config.InactiveGradientMidColor);
            _darkActiveGradientMidColor = UIntToColor(config.ActiveGradientMidColorDark);
            _darkInactiveGradientMidColor = UIntToColor(config.InactiveGradientMidColorDark);
            SelectedGradientIndex = config.TitleGradient;
            GradientMidEnabled = config.TitleGradientMid;
            GradientMidPos = config.TitleGradientMidPos;
            OnPropertyChanged(nameof(ActiveGradientMidColor));
            OnPropertyChanged(nameof(InactiveGradientMidColor));

            OnPropertyChanged(nameof(ActiveTitleBarColor));
            OnPropertyChanged(nameof(InactiveTitleBarColor));
            OnPropertyChanged(nameof(ActiveTextColor));
            OnPropertyChanged(nameof(InactiveTextColor));
            OnPropertyChanged(nameof(ActiveGradientColor));
            OnPropertyChanged(nameof(InactiveGradientColor));
            OnPropertyChanged(nameof(TitleBarGradientBrush));
        }

        private void SaveToConfig()
        {
            var config = _configManager.Config;
            config.ApplyGlobal = OverrideDwmApi;
            config.ExtendBorder = ExtendToBorder;
            config.Reflection = EnableAeroReflection;
            config.OldBtnHeight = RestoreWin7ButtonSize;
            config.TitleBtnGlow = EnableWin7ButtonGlow;
            config.CustomAmount = CustomAmount;
            config.BlurAmount = BlurRadius;

            config.ActiveBlendColor = ColorToUInt(_lightActiveTitleBarColor);
            config.InactiveBlendColor = ColorToUInt(_lightInactiveTitleBarColor);
            config.ActiveTextColor = ColorToUInt(_lightActiveTextColor);
            config.InactiveTextColor = ColorToUInt(_lightInactiveTextColor);

            config.ActiveBlendColorDark = ColorToUInt(_darkActiveTitleBarColor);
            config.InactiveBlendColorDark = ColorToUInt(_darkInactiveTitleBarColor);
            config.ActiveTextColorDark = ColorToUInt(_darkActiveTextColor);
            config.InactiveTextColorDark = ColorToUInt(_darkInactiveTextColor);

            config.TitleGradient = SelectedGradientIndex;
            config.ActiveGradientColor = ColorToUInt(_lightActiveGradientColor);
            config.InactiveGradientColor = ColorToUInt(_lightInactiveGradientColor);
            config.ActiveGradientColorDark = ColorToUInt(_darkActiveGradientColor);
            config.InactiveGradientColorDark = ColorToUInt(_darkInactiveGradientColor);
            config.TitleGradientMid = GradientMidEnabled;
            config.TitleGradientMidPos = GradientMidPos;
            config.ActiveGradientMidColor = ColorToUInt(_lightActiveGradientMidColor);
            config.InactiveGradientMidColor = ColorToUInt(_lightInactiveGradientMidColor);
            config.ActiveGradientMidColorDark = ColorToUInt(_darkActiveGradientMidColor);
            config.InactiveGradientMidColorDark = ColorToUInt(_darkInactiveGradientMidColor);

            _configManager.SaveConfig();
        }

        private Color UIntToColor(uint color)
        {
            // Format: 0xAARRGGBB
            return Color.FromArgb((byte)(color >> 24), (byte)color, (byte)(color >> 8), (byte)(color >> 16));
        }

        private uint ColorToUInt(Color color)
        {
            // Format: 0xAARRGGBB
            return ((uint)color.A << 24) | ((uint)color.B << 16) | ((uint)color.G << 8) | color.R;
        }

        [ObservableProperty]
        private ImageSource? _wallpaperSource;

        private void LoadWallpaper()
        {
            try
            {
                var registryKey = Registry.CurrentUser.OpenSubKey(@"Control Panel\Desktop");
                if (registryKey != null)
                {
                    var wallpaperPath = registryKey.GetValue("Wallpaper")?.ToString();
                    if (!string.IsNullOrEmpty(wallpaperPath) && System.IO.File.Exists(wallpaperPath))
                    {
                        var bitmap = new BitmapImage();
                        bitmap.BeginInit();
                        bitmap.CacheOption = BitmapCacheOption.OnLoad;
                        bitmap.UriSource = new Uri(wallpaperPath);
                        bitmap.EndInit();
                        bitmap.Freeze();
                        WallpaperSource = bitmap;
                    }
                }
            }
            catch
            {
                // Ignore errors
            }
        }

        [ObservableProperty]
        private string _pageTitle = "通用设置";

        [ObservableProperty]
        private string _statusText = "未安装";

        [ObservableProperty]
        [NotifyPropertyChangedFor(nameof(CanInstall))]
        private bool _isInstalled = false;

        public bool CanInstall => !IsInstalled;

        [ObservableProperty]
        private bool _overrideDwmApi = true;

        partial void OnOverrideDwmApiChanged(bool value)
        {
            _configManager.Config.ApplyGlobal = value;
        }

        [ObservableProperty]
        private bool _extendToBorder = false;

        partial void OnExtendToBorderChanged(bool value)
        {
            _configManager.Config.ExtendBorder = value;
        }

        [ObservableProperty]
        private bool _enableAeroReflection = true;

        partial void OnEnableAeroReflectionChanged(bool value)
        {
            _configManager.Config.Reflection = value;
        }

        [ObservableProperty]
        private bool _restoreWin7ButtonSize = true;

        partial void OnRestoreWin7ButtonSizeChanged(bool value)
        {
            _configManager.Config.OldBtnHeight = value;
        }

        public string TitleBtnSizePresetName
        {
            get
            {
                var config = _configManager.Config;
                if (!config.CustomTitleBtnSize)
                    return "Windows 7";
                
                // Use saved preset index
                return config.TitleBtnSizePreset switch
                {
                    0 => "Windows 7",
                    1 => "Windows Vista",
                    _ => Lang["custom"]
                };
            }
        }

        [RelayCommand]
        private void NavigateToTitleBtnSizeSettings()
        {
            AdvancedSettingsPage.ScrollToTitleBtnSizeOnLoad = true;
            _navigationService.Navigate(typeof(AdvancedSettingsPage));
        }

        [ObservableProperty]
        private bool _enableWin7ButtonGlow = false;

        partial void OnEnableWin7ButtonGlowChanged(bool value)
        {
            _configManager.Config.TitleBtnGlow = value;
        }

        [ObservableProperty]
        private bool _customAmount = false;

        partial void OnCustomAmountChanged(bool value)
        {
            _configManager.Config.CustomAmount = value;
        }

        [ObservableProperty]
        private int _blurRadius = 15;

        partial void OnBlurRadiusChanged(int value)
        {
            _configManager.Config.BlurAmount = value;
        }

        [ObservableProperty]
        private int _selectedThemeIndex = 0;

        // Light Theme Colors Storage
        private Color _lightActiveTitleBarColor = Color.FromArgb(100, 255, 66, 220);
        private Color _lightInactiveTitleBarColor = Color.FromArgb(100, 62, 239, 59);
        private Color _lightActiveTextColor = Color.FromRgb(0, 0, 0);
        private Color _lightInactiveTextColor = Color.FromRgb(180, 180, 180);

        // Dark Theme Colors Storage
        private Color _darkActiveTitleBarColor = Color.FromArgb(100, 0, 0, 0);
        private Color _darkInactiveTitleBarColor = Color.FromArgb(100, 30, 30, 30);
        private Color _darkActiveTextColor = Color.FromRgb(255, 255, 255);
        private Color _darkInactiveTextColor = Color.FromRgb(150, 150, 150);

        // Proxy Properties
        public Color ActiveTitleBarColor
        {
            get => SelectedThemeIndex == 0 ? _lightActiveTitleBarColor : _darkActiveTitleBarColor;
            set
            {
                if (SelectedThemeIndex == 0)
                {
                    if (_lightActiveTitleBarColor != value)
                    {
                        _lightActiveTitleBarColor = value;
                        _configManager.Config.ActiveBlendColor = ColorToUInt(value);
                        OnPropertyChanged();
                    }
                }
                else
                {
                    if (_darkActiveTitleBarColor != value)
                    {
                        _darkActiveTitleBarColor = value;
                        _configManager.Config.ActiveBlendColorDark = ColorToUInt(value);
                        OnPropertyChanged();
                    }
                }
            }
        }

        public Color InactiveTitleBarColor
        {
            get => SelectedThemeIndex == 0 ? _lightInactiveTitleBarColor : _darkInactiveTitleBarColor;
            set
            {
                if (SelectedThemeIndex == 0)
                {
                    if (_lightInactiveTitleBarColor != value)
                    {
                        _lightInactiveTitleBarColor = value;
                        _configManager.Config.InactiveBlendColor = ColorToUInt(value);
                        OnPropertyChanged();
                    }
                }
                else
                {
                    if (_darkInactiveTitleBarColor != value)
                    {
                        _darkInactiveTitleBarColor = value;
                        _configManager.Config.InactiveBlendColorDark = ColorToUInt(value);
                        OnPropertyChanged();
                    }
                }
            }
        }

        public Color ActiveTextColor
        {
            get => SelectedThemeIndex == 0 ? _lightActiveTextColor : _darkActiveTextColor;
            set
            {
                if (SelectedThemeIndex == 0)
                {
                    if (_lightActiveTextColor != value)
                    {
                        _lightActiveTextColor = value;
                        _configManager.Config.ActiveTextColor = ColorToUInt(value);
                        OnPropertyChanged();
                    }
                }
                else
                {
                    if (_darkActiveTextColor != value)
                    {
                        _darkActiveTextColor = value;
                        _configManager.Config.ActiveTextColorDark = ColorToUInt(value);
                        OnPropertyChanged();
                    }
                }
            }
        }

        public Color InactiveTextColor
        {
            get => SelectedThemeIndex == 0 ? _lightInactiveTextColor : _darkInactiveTextColor;
            set
            {
                if (SelectedThemeIndex == 0)
                {
                    if (_lightInactiveTextColor != value)
                    {
                        _lightInactiveTextColor = value;
                        _configManager.Config.InactiveTextColor = ColorToUInt(value);
                        OnPropertyChanged();
                    }
                }
                else
                {
                    if (_darkInactiveTextColor != value)
                    {
                        _darkInactiveTextColor = value;
                        _configManager.Config.InactiveTextColorDark = ColorToUInt(value);
                        OnPropertyChanged();
                    }
                }
            }
        }

        // Titlebar gradient. Index matches the config value:
        // 0 = off, 1 = horizontal (Windows 98), 2 = vertical (XP Media Center).
        [ObservableProperty]
        [NotifyPropertyChangedFor(nameof(IsGradientEnabled))]
        [NotifyPropertyChangedFor(nameof(IsGradientMidEnabled))]
        [NotifyPropertyChangedFor(nameof(TitleBarGradientBrush))]
        private int _selectedGradientIndex = 0;

        partial void OnSelectedGradientIndexChanged(int value)
        {
            _configManager.Config.TitleGradient = value;
        }

        public bool IsGradientEnabled => SelectedGradientIndex != 0;

        private Color _lightActiveGradientColor = Color.FromRgb(16, 132, 208);
        private Color _lightInactiveGradientColor = Color.FromRgb(181, 181, 181);
        private Color _darkActiveGradientColor = Color.FromRgb(16, 132, 208);
        private Color _darkInactiveGradientColor = Color.FromRgb(80, 80, 80);

        public Color ActiveGradientColor
        {
            get => SelectedThemeIndex == 0 ? _lightActiveGradientColor : _darkActiveGradientColor;
            set
            {
                if (SelectedThemeIndex == 0)
                {
                    if (_lightActiveGradientColor == value) return;
                    _lightActiveGradientColor = value;
                    _configManager.Config.ActiveGradientColor = ColorToUInt(value);
                }
                else
                {
                    if (_darkActiveGradientColor == value) return;
                    _darkActiveGradientColor = value;
                    _configManager.Config.ActiveGradientColorDark = ColorToUInt(value);
                }
                OnPropertyChanged();
                OnPropertyChanged(nameof(TitleBarGradientBrush));
            }
        }

        public Color InactiveGradientColor
        {
            get => SelectedThemeIndex == 0 ? _lightInactiveGradientColor : _darkInactiveGradientColor;
            set
            {
                if (SelectedThemeIndex == 0)
                {
                    if (_lightInactiveGradientColor == value) return;
                    _lightInactiveGradientColor = value;
                    _configManager.Config.InactiveGradientColor = ColorToUInt(value);
                }
                else
                {
                    if (_darkInactiveGradientColor == value) return;
                    _darkInactiveGradientColor = value;
                    _configManager.Config.InactiveGradientColorDark = ColorToUInt(value);
                }
                OnPropertyChanged();
            }
        }

        // Optional middle point: blend colour -> middle -> end colour.
        [ObservableProperty]
        [NotifyPropertyChangedFor(nameof(TitleBarGradientBrush))]
        [NotifyPropertyChangedFor(nameof(IsGradientMidEnabled))]
        private bool _gradientMidEnabled = false;

        partial void OnGradientMidEnabledChanged(bool value)
        {
            _configManager.Config.TitleGradientMid = value;
        }

        [ObservableProperty]
        [NotifyPropertyChangedFor(nameof(TitleBarGradientBrush))]
        private int _gradientMidPos = 64;

        partial void OnGradientMidPosChanged(int value)
        {
            _configManager.Config.TitleGradientMidPos = value;
        }

        public bool IsGradientMidEnabled => IsGradientEnabled && GradientMidEnabled;

        private Color _lightActiveGradientMidColor = Color.FromRgb(56, 119, 199);
        private Color _lightInactiveGradientMidColor = Color.FromRgb(150, 150, 150);
        private Color _darkActiveGradientMidColor = Color.FromRgb(56, 119, 199);
        private Color _darkInactiveGradientMidColor = Color.FromRgb(64, 64, 64);

        public Color ActiveGradientMidColor
        {
            get => SelectedThemeIndex == 0 ? _lightActiveGradientMidColor : _darkActiveGradientMidColor;
            set
            {
                if (SelectedThemeIndex == 0)
                {
                    if (_lightActiveGradientMidColor == value) return;
                    _lightActiveGradientMidColor = value;
                    _configManager.Config.ActiveGradientMidColor = ColorToUInt(value);
                }
                else
                {
                    if (_darkActiveGradientMidColor == value) return;
                    _darkActiveGradientMidColor = value;
                    _configManager.Config.ActiveGradientMidColorDark = ColorToUInt(value);
                }
                OnPropertyChanged();
                OnPropertyChanged(nameof(TitleBarGradientBrush));
            }
        }

        public Color InactiveGradientMidColor
        {
            get => SelectedThemeIndex == 0 ? _lightInactiveGradientMidColor : _darkInactiveGradientMidColor;
            set
            {
                if (SelectedThemeIndex == 0)
                {
                    if (_lightInactiveGradientMidColor == value) return;
                    _lightInactiveGradientMidColor = value;
                    _configManager.Config.InactiveGradientMidColor = ColorToUInt(value);
                }
                else
                {
                    if (_darkInactiveGradientMidColor == value) return;
                    _darkInactiveGradientMidColor = value;
                    _configManager.Config.InactiveGradientMidColorDark = ColorToUInt(value);
                }
                OnPropertyChanged();
            }
        }

        // Preview overlay for the sample titlebar, drawn the way DWM draws it:
        // clear at the start edge (so the blend colour shows), then the middle
        // point if on, then the end colour, all on top of the blend colour.
        // The clear stop keeps the next stop's RGB so the bar does not darken.
        public Brush TitleBarGradientBrush
        {
            get
            {
                if (SelectedGradientIndex == 0)
                    return Brushes.Transparent;

                var end = ActiveGradientColor;
                var mid = ActiveGradientMidColor;
                var next = GradientMidEnabled ? mid : end;
                var brush = new LinearGradientBrush
                {
                    StartPoint = new System.Windows.Point(0, 0),
                    EndPoint = SelectedGradientIndex == 1 ? new System.Windows.Point(1, 0) : new System.Windows.Point(0, 1)
                };
                brush.GradientStops.Add(new GradientStop(Color.FromArgb(0, next.R, next.G, next.B), 0));
                if (GradientMidEnabled)
                    brush.GradientStops.Add(new GradientStop(mid, GradientMidPos / 100.0));
                brush.GradientStops.Add(new GradientStop(end, 1));
                brush.Freeze();
                return brush;
            }
        }

        public Brush ContentAreaBrush => SelectedThemeIndex == 0
            ? new SolidColorBrush(Color.FromArgb(100, 255, 255, 255))
            : new SolidColorBrush(Color.FromArgb(100, 0, 0, 0));

        partial void OnSelectedThemeIndexChanged(int value)
        {
            OnPropertyChanged(nameof(ActiveTitleBarColor));
            OnPropertyChanged(nameof(InactiveTitleBarColor));
            OnPropertyChanged(nameof(ActiveTextColor));
            OnPropertyChanged(nameof(InactiveTextColor));
            OnPropertyChanged(nameof(ActiveGradientColor));
            OnPropertyChanged(nameof(InactiveGradientColor));
            OnPropertyChanged(nameof(ActiveGradientMidColor));
            OnPropertyChanged(nameof(InactiveGradientMidColor));
            OnPropertyChanged(nameof(TitleBarGradientBrush));
            OnPropertyChanged(nameof(ContentAreaBrush));
        }

        [RelayCommand]
        private async Task OnInstall()
        {
            // First save current config
            SaveToConfig();
            _configManager.SaveConfig();

            var (success, errorMessage) = TaskSchedulerService.InstallTask();
            if (success)
            {
                IsInstalled = true;
                StatusText = Lang["status1"];

                // Check if symbols are valid, if not show warning
                if (!_symbolDownloader.CheckSymbolsValid())
                {
                    await ShowMessageDialog("Info", Lang["installsucs1"]);
                }
                else
                {
                    // Start the host process
                    TaskSchedulerService.RunHostProcess();
                }
            }
            else
            {
                await ShowMessageDialog("Error", Lang["installfail"] + (errorMessage != null ? $"{errorMessage}" : ""));
            }
        }

        [RelayCommand]
        private void OnUninstall()
        {
            // Stop the running process first
            TaskSchedulerService.StopHostProcess();

            var (success, errorMessage) = TaskSchedulerService.UninstallTask();
            if (success)
            {
                TaskSchedulerService.RemoveNotificationRegistration();
                IsInstalled = false;
                StatusText = Lang["status0"];
            }
            else
            {
                // Show error only on failure
                _ = ShowMessageDialog("Error", Lang["uninstallfail"] + (errorMessage != null ? $"{errorMessage}" : ""));
            }
        }

        private async Task ShowMessageDialog(string title, string message)
        {
            var dialogHost = _contentDialogService.GetDialogHost();
            if (dialogHost == null) return;

            var dialog = new ContentDialog
            {
                Title = title,
                Content = message,
                CloseButtonText = "OK",
                DialogHost = dialogHost
            };

            await dialog.ShowAsync();
        }

        [RelayCommand(CanExecute = nameof(CanSaveConfig))]
        private void OnSaveConfig()
        {
            SaveToConfig();
            
            if (IsInstalled)
            {
                TaskSchedulerService.RefreshDwmConfig();
            }
        }

        private bool CanSaveConfig() => _configManager.IsModified;

        [RelayCommand]
        private async Task OnPickColor(string colorType)
        {
            Color currentColor = colorType switch
            {
                "ActiveTitleBar" => ActiveTitleBarColor,
                "InactiveTitleBar" => InactiveTitleBarColor,
                "ActiveText" => ActiveTextColor,
                "InactiveText" => InactiveTextColor,
                "ActiveGradient" => ActiveGradientColor,
                "InactiveGradient" => InactiveGradientColor,
                "ActiveGradientMid" => ActiveGradientMidColor,
                "InactiveGradientMid" => InactiveGradientMidColor,
                _ => Colors.White
            };

            bool isRgbMode = colorType == "ActiveText" || colorType == "InactiveText";

            var dialogHost = _contentDialogService.GetDialogHost();
            var dialog = new ColorPickerDialog(dialogHost, currentColor, isRgbMode);

            var result = await dialog.ShowAsync();

            if (result == ContentDialogResult.Primary)
            {
                switch (colorType)
                {
                    case "ActiveTitleBar":
                        ActiveTitleBarColor = dialog.SelectedColor;
                        break;
                    case "InactiveTitleBar":
                        InactiveTitleBarColor = dialog.SelectedColor;
                        break;
                    case "ActiveText":
                        ActiveTextColor = dialog.SelectedColor;
                        break;
                    case "InactiveText":
                        InactiveTextColor = dialog.SelectedColor;
                        break;
                    case "ActiveGradient":
                        ActiveGradientColor = dialog.SelectedColor;
                        break;
                    case "InactiveGradient":
                        InactiveGradientColor = dialog.SelectedColor;
                        break;
                    case "ActiveGradientMid":
                        ActiveGradientMidColor = dialog.SelectedColor;
                        break;
                    case "InactiveGradientMid":
                        InactiveGradientMidColor = dialog.SelectedColor;
                        break;
                }
            }
        }

        // Enabled state properties based on BlurMethod
        // CustomBlur (0): All enabled
        // OldBlur/AccentBlur (1): All enabled except EnableWin7ButtonGlow
        // DWMAPI (2): All disabled except install module
        private BlurMethod CurrentBlurMethod => _configManager.Config.BlurMethod;
        
        public bool IsEffectsSettingsEnabled => CurrentBlurMethod != BlurMethod.DWMAPIBlur;
        public bool IsColorsSettingsEnabled => CurrentBlurMethod != BlurMethod.DWMAPIBlur;
        public bool IsPreviewEnabled => CurrentBlurMethod != BlurMethod.DWMAPIBlur;
        public bool IsWin7ButtonGlowEnabled => CurrentBlurMethod == BlurMethod.CustomBlur;

        private void UpdateEnabledStates()
        {
            OnPropertyChanged(nameof(IsEffectsSettingsEnabled));
            OnPropertyChanged(nameof(IsColorsSettingsEnabled));
            OnPropertyChanged(nameof(IsPreviewEnabled));
            OnPropertyChanged(nameof(IsWin7ButtonGlowEnabled));
        }
    }
}
