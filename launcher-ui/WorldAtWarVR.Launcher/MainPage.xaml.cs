using System.Diagnostics;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Windows.UI.ViewManagement;
using Windows.Storage.Pickers;
using WorldAtWarVR.Launcher.Core;

namespace WorldAtWarVR_Launcher;

public sealed partial class MainPage : Page
{
    private readonly LauncherSettingsStore _settingsStore = new();
    private readonly GitHubUpdateService _updateService = new();
    private readonly SemanticVersion _installedVersion = InstalledVersionProvider.Get(typeof(App).Assembly);
    private readonly DispatcherQueueTimer _validationTimer;
    private LauncherSettings _settings = new();
    private GameInstallationValidation? _validation;
    private CancellationTokenSource? _validationCancellation;
    private CancellationTokenSource? _updateCancellation;
    private AvailableUpdate? _availableUpdate;
    private UpdateReleaseSummary? _visibleUpdate;
    private bool _initializing = true;
    private bool _launching;
    private bool _startupUpdateFlowStarted;
    private bool _updateDialogOpen;
    private bool _updating;

    public bool IsHighContrast { get; } = new AccessibilitySettings().HighContrast;

    public MainPage()
    {
        InitializeComponent();
        _validationTimer = DispatcherQueue.CreateTimer();
        _validationTimer.Interval = TimeSpan.FromMilliseconds(550);
        _validationTimer.IsRepeating = false;
        _validationTimer.Tick += ValidationTimer_Tick;
        Loaded += MainPage_Loaded;
        Unloaded += MainPage_Unloaded;
    }

    private async void MainPage_Loaded(object sender, RoutedEventArgs e)
    {
        _settings = await _settingsStore.LoadAsync();
        if (string.IsNullOrWhiteSpace(_settings.GameDirectory))
        {
            SetActivity("Looking for your Steam installation…", busy: true);
            var detected = await Task.Run(SteamInstallDetector.FindCandidateGameDirectories);
            var validated = await Task.WhenAll(
                detected.Select(path => GameInstallationValidator.ValidateAsync(path)));
            var best = GameInstallationValidator.ChooseBestCandidate(validated);
            _settings = _settings with { GameDirectory = best?.Directory ?? string.Empty };
        }

        ApplySettingsToControls();
        RestoreCachedUpdateNotice();
        _initializing = false;
        StartStartupUpdateFlow();
        await ValidateCurrentDirectoryAsync();
        await SaveSettingsQuietlyAsync();
    }

    private void MainPage_Unloaded(object sender, RoutedEventArgs e)
    {
        _validationTimer.Stop();
        _validationCancellation?.Cancel();
        _validationCancellation?.Dispose();
        _updateCancellation?.Cancel();
        _updateCancellation?.Dispose();
        _updateCancellation = null;
    }

    private void ApplySettingsToControls()
    {
        GamePathTextBox.Text = _settings.GameDirectory;
        MainMenuRadio.IsChecked = _settings.LaunchTarget == GameLaunchTarget.MainMenu;
        MultiplayerRadio.IsChecked = _settings.LaunchTarget == GameLaunchTarget.Multiplayer;
        BotsCheckBox.IsChecked = _settings.AutomaticBots;
        AirLinkSteamVrCheckBox.IsChecked = _settings.QuestAirLinkCompatibility;
        PerformanceRadio.IsChecked = _settings.Resolution == VrResolutionPreset.Performance;
        RecommendedRadio.IsChecked = _settings.Resolution == VrResolutionPreset.Recommended;
        HighQualityRadio.IsChecked = _settings.Resolution == VrResolutionPreset.HighQuality;
        UpdateTargetPresentation();
        UpdateResolutionPresentation();
    }

    private LauncherSettings ReadSettingsFromControls()
    {
        return (_settings with
        {
            GameDirectory = GamePathTextBox.Text,
            LaunchTarget = MultiplayerRadio.IsChecked == true
                ? GameLaunchTarget.Multiplayer
                : GameLaunchTarget.MainMenu,
            AutomaticBots = BotsCheckBox.IsChecked == true,
            QuestAirLinkCompatibility = AirLinkSteamVrCheckBox.IsChecked == true,
            Resolution = GetSelectedResolution(),
        }).Normalize();
    }

    private void RestoreCachedUpdateNotice()
    {
        if (_settings.CachedUpdate?.TryGetRelease(out var cachedRelease) == true &&
            cachedRelease!.Version > _installedVersion)
        {
            SetVisibleUpdate(cachedRelease);
            return;
        }

        _settings = _settings with { CachedUpdate = null };
        SetVisibleUpdate(null);
    }

    private void StartStartupUpdateFlow()
    {
        if (_startupUpdateFlowStarted)
        {
            return;
        }

        _startupUpdateFlowStarted = true;
        _updateCancellation = new CancellationTokenSource();
        var token = _updateCancellation.Token;
        DispatcherQueue.TryEnqueue(
            DispatcherQueuePriority.Low,
            () => _ = RunStartupUpdateFlowAsync(token));
    }

    private async Task RunStartupUpdateFlowAsync(CancellationToken cancellationToken)
    {
        try
        {
            var now = DateTimeOffset.UtcNow;
            if (!GitHubUpdateService.ShouldCheck(_settings.LastUpdateCheckUtc, now))
            {
                if (_visibleUpdate is not null &&
                    !GitHubUpdateService.IsPromptSnoozed(_settings.UpdatePromptSnoozedUntilUtc, now))
                {
                    await ShowUpdateDialogAsync(_visibleUpdate);
                }

                return;
            }

            var update = await _updateService.FindAvailableUpdateAsync(
                _installedVersion,
                cancellationToken);
            cancellationToken.ThrowIfCancellationRequested();

            _availableUpdate = update;
            SetVisibleUpdate(update?.Release);
            _settings = _settings with
            {
                LastUpdateCheckUtc = DateTimeOffset.UtcNow,
                CachedUpdate = update is null
                    ? null
                    : CachedUpdateInfo.FromRelease(update.Release),
            };
            await SaveSettingsSnapshotQuietlyAsync(reportFailure: false);

            if (update is not null &&
                !GitHubUpdateService.IsPromptSnoozed(
                    _settings.UpdatePromptSnoozedUntilUtc,
                    DateTimeOffset.UtcNow))
            {
                await ShowUpdateDialogAsync(update.Release);
            }
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            // Closing the launcher cancels an in-flight check or download.
        }
        catch (Exception exception)
        {
            // Update discovery is advisory. Offline, rate-limited, and malformed
            // responses must never affect launcher startup or game launch.
            Debug.WriteLine($"World War VR update check failed: {exception}");
        }
    }

    private void SetVisibleUpdate(UpdateReleaseSummary? release)
    {
        _visibleUpdate = release;
        UpdateAvailableButton.Visibility = release is null
            ? Visibility.Collapsed
            : Visibility.Visible;
        UpdateAvailableButton.Content = release is null
            ? "Update available"
            : $"Update available — {release.Version}";
    }

    private async void UpdateAvailableButton_Click(object sender, RoutedEventArgs e)
    {
        if (_visibleUpdate is null)
        {
            return;
        }

        try
        {
            await ShowUpdateDialogAsync(_visibleUpdate);
        }
        catch (Exception exception)
        {
            Debug.WriteLine($"World War VR update dialog failed: {exception}");
            ShowActivityError("Update details could not be opened. You can still launch World War VR.");
        }
    }

    private async Task ShowUpdateDialogAsync(UpdateReleaseSummary release)
    {
        if (_updateDialogOpen || _updating || XamlRoot is null)
        {
            return;
        }

        var notes = new TextBlock
        {
            Text = release.ReleaseNotes,
            TextWrapping = TextWrapping.Wrap,
            IsTextSelectionEnabled = true,
        };
        var dialogContent = new StackPanel
        {
            Spacing = 12,
        };
        dialogContent.Children.Add(new TextBlock
        {
            Text = $"Version {release.Version}",
            Style = (Style)Application.Current.Resources["SecondaryTextStyle"],
        });
        dialogContent.Children.Add(new ScrollViewer
        {
            MaxHeight = 360,
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            VerticalScrollMode = ScrollMode.Auto,
            Content = notes,
        });
        var snoozeCheckBox = new CheckBox
        {
            Content = "Don't show this popup for 30 days",
            IsChecked = false,
        };
        dialogContent.Children.Add(snoozeCheckBox);

        var dialog = new ContentDialog
        {
            XamlRoot = XamlRoot,
            Title = release.Title,
            Content = dialogContent,
            PrimaryButtonText = "Update now",
            SecondaryButtonText = "Later",
            DefaultButton = ContentDialogButton.Primary,
        };

        ContentDialogResult result;
        _updateDialogOpen = true;
        try
        {
            result = await dialog.ShowAsync();
        }
        finally
        {
            _updateDialogOpen = false;
        }

        if (result == ContentDialogResult.Primary)
        {
            await DownloadAndStartInstallerAsync(release);
        }
        else if (snoozeCheckBox.IsChecked == true)
        {
            _settings = ReadSettingsFromControls() with
            {
                UpdatePromptSnoozedUntilUtc =
                    DateTimeOffset.UtcNow + GitHubUpdateService.PromptSnoozeDuration,
            };
            await SaveSettingsSnapshotQuietlyAsync(reportFailure: true);
        }
    }

    private async Task DownloadAndStartInstallerAsync(UpdateReleaseSummary requestedRelease)
    {
        if (_updating)
        {
            return;
        }

        _updating = true;
        UpdateAvailableButton.IsEnabled = false;
        UpdateAvailableButton.Content = "Preparing update…";
        var cancellationToken = _updateCancellation?.Token ?? CancellationToken.None;
        try
        {
            var update = _availableUpdate;
            if (update is null || !IsSameRelease(update.Release, requestedRelease))
            {
                update = await _updateService.FindAvailableUpdateAsync(
                    _installedVersion,
                    cancellationToken);
            }

            if (update is null)
            {
                throw new InvalidOperationException("The selected release is no longer available.");
            }

            if (!IsSameRelease(update.Release, requestedRelease))
            {
                _availableUpdate = update;
                SetVisibleUpdate(update.Release);
                _settings = ReadSettingsFromControls() with
                {
                    LastUpdateCheckUtc = DateTimeOffset.UtcNow,
                    CachedUpdate = CachedUpdateInfo.FromRelease(update.Release),
                };
                await SaveSettingsSnapshotQuietlyAsync(reportFailure: false);
                ShowActivityError("A newer update was found. Open Update available again to review it.");
                return;
            }

            _availableUpdate = update;
            var progress = new Progress<UpdateDownloadProgress>(value =>
            {
                UpdateAvailableButton.Content = $"Downloading update… {value.Percentage}%";
            });
            var installerPath = await _updateService.DownloadInstallerAsync(
                update,
                progress,
                cancellationToken);

            var installer = new FileInfo(installerPath);
            if (!installer.Exists ||
                installer.Length != update.Asset.Size ||
                !installer.Name.Equals(GitHubUpdateService.InstallerFileName, StringComparison.Ordinal))
            {
                throw new InvalidDataException("The verified update installer is no longer available.");
            }

            await _updateService.VerifyInstallerAsync(
                installer.FullName,
                update.Asset,
                cancellationToken);

            var startInfo = new ProcessStartInfo
            {
                FileName = installer.FullName,
                WorkingDirectory = installer.DirectoryName!,
                UseShellExecute = true,
            };
            using var installerProcess = Process.Start(startInfo)
                ?? throw new InvalidOperationException("The verified update installer could not be started.");

            App.MainWindowInstance?.Close();
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            // Window shutdown intentionally cancels update work.
        }
        catch (Exception exception)
        {
            Debug.WriteLine($"World War VR update failed: {exception}");
            ShowActivityError("The update could not be downloaded or started. You can still launch World War VR.");
        }
        finally
        {
            _updating = false;
            UpdateAvailableButton.IsEnabled = true;
            if (_visibleUpdate is not null)
            {
                UpdateAvailableButton.Content = $"Update available — {_visibleUpdate.Version}";
            }
        }
    }

    private static bool IsSameRelease(UpdateReleaseSummary left, UpdateReleaseSummary right) =>
        left.Version.Equals(right.Version) &&
        left.TagName.Equals(right.TagName, StringComparison.Ordinal);

    private void GamePathTextBox_TextChanged(object sender, TextChangedEventArgs e)
    {
        if (_initializing)
        {
            return;
        }

        _validationTimer.Stop();
        _validationTimer.Start();
        ActivityStatusText.Text = "Waiting to check the selected folder…";
    }

    private async void ValidationTimer_Tick(DispatcherQueueTimer sender, object args)
    {
        await ValidateCurrentDirectoryAsync();
        await SaveSettingsQuietlyAsync();
    }

    private async void BrowseButton_Click(object sender, RoutedEventArgs e)
    {
        var window = App.MainWindowInstance;
        if (window is null)
        {
            ShowActivityError("The folder picker could not be opened. Restart the launcher and try again.");
            return;
        }

        var picker = new FolderPicker
        {
            SuggestedStartLocation = PickerLocationId.ComputerFolder,
        };
        picker.FileTypeFilter.Add("*");
        WinRT.Interop.InitializeWithWindow.Initialize(
            picker, WinRT.Interop.WindowNative.GetWindowHandle(window));

        var folder = await picker.PickSingleFolderAsync();
        if (folder is null)
        {
            return;
        }

        _validationTimer.Stop();
        GamePathTextBox.Text = folder.Path;
        await ValidateCurrentDirectoryAsync();
        await SaveSettingsQuietlyAsync();
    }

    private async void LaunchTarget_Checked(object sender, RoutedEventArgs e)
    {
        if (_initializing)
        {
            return;
        }

        UpdateTargetPresentation();
        UpdateValidationPresentation();
        await SaveSettingsQuietlyAsync();
    }

    private async void SettingControl_Changed(object sender, RoutedEventArgs e)
    {
        UpdateLaunchSummary();
        if (!_initializing)
        {
            await SaveSettingsQuietlyAsync();
        }
    }

    private async void ResolutionRadio_Checked(object sender, RoutedEventArgs e)
    {
        UpdateResolutionPresentation();
        if (!_initializing)
        {
            await SaveSettingsQuietlyAsync();
        }
    }

    private void UpdateTargetPresentation()
    {
        var multiplayer = MultiplayerRadio.IsChecked == true;
        MultiplayerInfoBar.IsOpen = multiplayer;
        UpdateLaunchSummary();
    }

    private void UpdateResolutionPresentation()
    {
        var choice = ResolutionChoices.Get(GetSelectedResolution());
        ResolutionDetailText.Text = $"{choice.Detail} · {choice.Width} × {choice.Height}";
        UpdateLaunchSummary();
    }

    private VrResolutionPreset GetSelectedResolution()
    {
        if (PerformanceRadio.IsChecked == true)
        {
            return VrResolutionPreset.Performance;
        }

        return HighQualityRadio.IsChecked == true
            ? VrResolutionPreset.HighQuality
            : VrResolutionPreset.Recommended;
    }

    private async Task ValidateCurrentDirectoryAsync()
    {
        _validationCancellation?.Cancel();
        _validationCancellation?.Dispose();
        _validationCancellation = new CancellationTokenSource();
        var token = _validationCancellation.Token;

        SetActivity("Checking the game files…", busy: true);
        try
        {
            _validation = await GameInstallationValidator.ValidateAsync(GamePathTextBox.Text, token);
            if (!token.IsCancellationRequested)
            {
                UpdateValidationPresentation();
                SetActivity("Settings are saved automatically.", busy: false);
            }
        }
        catch (OperationCanceledException)
        {
            // A newer folder selection superseded this validation.
        }
    }

    private void UpdateValidationPresentation()
    {
        if (_validation is null)
        {
            InstallationInfoBar.Severity = InfoBarSeverity.Informational;
            InstallationInfoBar.Title = "Select your game";
            InstallationInfoBar.Message = string.Empty;
            LaunchButton.IsEnabled = false;
            InstallationStatusText.Text = "Select your game installation";
            InstallationStatusText.Foreground = ResourceBrush("LauncherSecondaryTextBrush");
            InstallationStatusRing.Stroke = ResourceBrush("LauncherBorderBrush");
            InstallationStatusIcon.Foreground = ResourceBrush("LauncherSecondaryTextBrush");
            UpdateLaunchSummary();
            return;
        }

        var target = MultiplayerRadio.IsChecked == true
            ? GameLaunchTarget.Multiplayer
            : GameLaunchTarget.MainMenu;
        var ready = _validation.IsReadyFor(target);
        var anyReady = _validation.MainMenuReady || _validation.MultiplayerReady;

        InstallationInfoBar.Severity = ready
            ? InfoBarSeverity.Success
            : anyReady ? InfoBarSeverity.Warning : InfoBarSeverity.Error;
        InstallationInfoBar.Title = ready
            ? "Installation ready"
            : anyReady ? "Selected target needs attention" : "Installation not supported";
        InstallationInfoBar.Message = _validation.ReadySummary;
        LaunchButton.IsEnabled = ready && !_launching;
        UpdateInstallationStatus(ready, anyReady);
        UpdateLaunchSummary();
    }

    private void UpdateInstallationStatus(bool ready, bool anyReady)
    {
        var steamBuild = MultiplayerRadio.IsChecked == true
            ? _validation?.MultiplayerBuild
            : _validation?.SinglePlayerBuild;
        InstallationStatusText.Text = ready
            ? steamBuild == SupportedGameBuild.SteamBuild252004
                ? "Steam installation verified"
                : "Compatible installation verified"
            : anyReady
                ? "Selected target needs attention"
                : _validation?.Detail ?? "Installation not supported";

        var brush = ready
            ? ResourceBrush("LauncherAccentBrush")
            : anyReady
                ? new SolidColorBrush(Microsoft.UI.Colors.Goldenrod)
                : new SolidColorBrush(Microsoft.UI.Colors.OrangeRed);
        InstallationStatusText.Foreground = brush;
        InstallationStatusRing.Stroke = brush;
        InstallationStatusIcon.Foreground = brush;
    }

    private void UpdateLaunchSummary()
    {
        var target = MultiplayerRadio.IsChecked == true
            ? GameLaunchTarget.Multiplayer
            : GameLaunchTarget.MainMenu;
        if (_validation?.IsReadyFor(target) != true)
        {
            LaunchSummaryText.Text = target == GameLaunchTarget.Multiplayer
                ? "Select an installation that is ready for Multiplayer."
                : "Select an installation that is ready for Zombies and Campaign.";
            return;
        }

        LaunchSummaryText.Text = AirLinkSteamVrCheckBox.IsChecked == true
            ? "Ready to launch in VR through SteamVR"
            : "Ready to launch in VR";
    }

    private async void LaunchButton_Click(object sender, RoutedEventArgs e)
    {
        if (_launching)
        {
            return;
        }

        await ValidateCurrentDirectoryAsync();
        var settings = ReadSettingsFromControls();
        if (_validation?.IsReadyFor(settings.LaunchTarget) != true)
        {
            ShowActivityError("The selected installation is not ready for this launch target.");
            return;
        }

        LauncherCommand command;
        IReadOnlyDictionary<string, string> launchEnvironment;
        try
        {
            command = LauncherCommandBuilder.Build(
                AppContext.BaseDirectory,
                settings,
                _validation);
            launchEnvironment = settings.QuestAirLinkCompatibility
                ? SteamVrAirLinkCompatibility.ResolveLaunchEnvironment()
                : new Dictionary<string, string>();
            var modDll = Path.Combine(AppContext.BaseDirectory, LauncherCommandBuilder.ModDllFileName);
            if (!File.Exists(command.FileName) || !File.Exists(modDll))
            {
                ShowActivityError("The VR support files are missing. Reinstall World War VR and try again.");
                return;
            }
        }
        catch (Exception exception) when (
            exception is ArgumentException or IOException or InvalidOperationException)
        {
            ShowActivityError(exception.Message);
            return;
        }

        _launching = true;
        LaunchButton.IsEnabled = false;
        SetActivity(
            settings.QuestAirLinkCompatibility
                ? "Starting Call of Duty: World at War through SteamVR…"
                : "Starting Call of Duty: World at War in VR…",
            busy: true);
        await SaveSettingsQuietlyAsync();

        try
        {
            var startInfo = new ProcessStartInfo
            {
                FileName = command.FileName,
                WorkingDirectory = AppContext.BaseDirectory,
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
            };
            foreach (var argument in command.Arguments)
            {
                startInfo.ArgumentList.Add(argument);
            }
            foreach (var variable in launchEnvironment)
            {
                startInfo.Environment[variable.Key] = variable.Value;
            }

            using var process = Process.Start(startInfo)
                ?? throw new InvalidOperationException("The VR launcher process could not be started.");
            var outputTask = process.StandardOutput.ReadToEndAsync();
            var errorTask = process.StandardError.ReadToEndAsync();
            await process.WaitForExitAsync();
            var output = await outputTask;
            var error = await errorTask;

            if (process.ExitCode != 0)
            {
                var detail = LastUsefulLine(error, output);
                ShowActivityError(string.IsNullOrWhiteSpace(detail)
                    ? $"World War VR could not start (error {process.ExitCode})."
                    : detail);
            }
            else
            {
                ActivityStatusText.Foreground = new SolidColorBrush(Microsoft.UI.Colors.LightGreen);
                SetActivity("World War VR started successfully.", busy: false);
            }
        }
        catch (Exception exception) when (exception is IOException or InvalidOperationException or UnauthorizedAccessException)
        {
            ShowActivityError($"World War VR could not start: {exception.Message}");
        }
        finally
        {
            _launching = false;
            UpdateValidationPresentation();
        }
    }

    private async Task SaveSettingsQuietlyAsync()
    {
        if (_initializing)
        {
            return;
        }

        _settings = ReadSettingsFromControls();
        await SaveSettingsSnapshotQuietlyAsync(reportFailure: true);
    }

    private async Task SaveSettingsSnapshotQuietlyAsync(bool reportFailure)
    {
        try
        {
            await _settingsStore.SaveAsync(_settings);
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
        {
            if (reportFailure)
            {
                ShowActivityError("Settings could not be saved, but you can still launch the game.");
            }
        }
    }

    private void SetActivity(string message, bool busy)
    {
        BusyProgressRing.IsActive = busy;
        BusyProgressRing.Visibility = busy ? Visibility.Visible : Visibility.Collapsed;
        ReadyStatusRing.Visibility = busy ? Visibility.Collapsed : Visibility.Visible;
        ReadyStatusIcon.Visibility = busy ? Visibility.Collapsed : Visibility.Visible;
        InstallationProgressRing.IsActive = busy;
        InstallationProgressRing.Visibility = busy ? Visibility.Visible : Visibility.Collapsed;
        InstallationStatusRing.Visibility = busy ? Visibility.Collapsed : Visibility.Visible;
        InstallationStatusIcon.Visibility = busy ? Visibility.Collapsed : Visibility.Visible;
        ActivityStatusText.Text = message;
        if (busy)
        {
            ActivityStatusText.ClearValue(TextBlock.ForegroundProperty);
        }
    }

    private void ShowActivityError(string message)
    {
        SetActivity(message, busy: false);
        ActivityStatusText.Foreground = new SolidColorBrush(Microsoft.UI.Colors.OrangeRed);
    }

    private static SolidColorBrush ResourceBrush(string key) =>
        (SolidColorBrush)Application.Current.Resources[key];

    private static string LastUsefulLine(params string[] messages)
    {
        foreach (var message in messages)
        {
            var line = message
                .Split(['\r', '\n'], StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)
                .LastOrDefault();
            if (!string.IsNullOrWhiteSpace(line))
            {
                return line;
            }
        }

        return string.Empty;
    }
}
