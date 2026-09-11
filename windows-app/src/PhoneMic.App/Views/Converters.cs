using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Data;

namespace PhoneMic.App.Views;

/// <summary>Конвертеры для биндингов Dashboard.</summary>
public sealed class BoolToVisConverter : IValueConverter
{
    public bool Negate { get; set; }
    public object Convert(object? value, Type targetType, object? parameter, string language)
    {
        bool b = value is bool v && v;
        if (Negate) b = !b;
        return b ? Visibility.Visible : Visibility.Collapsed;
    }
    public object ConvertBack(object? value, Type targetType, object? parameter, string language) =>
        throw new NotSupportedException();
}

public sealed class MuteLabelConverter : IValueConverter
{
    public object Convert(object? value, Type targetType, object? parameter, string language) =>
        value is true ? "Включить звук" : "Мьют";
    public object ConvertBack(object? value, Type targetType, object? parameter, string language) =>
        throw new NotSupportedException();
}

public sealed class ActiveLabelConverter : IValueConverter
{
    public object Convert(object? value, Type targetType, object? parameter, string language) =>
        value is true ? "Микрофон включён" : "Микрофон выключен";
    public object ConvertBack(object? value, Type targetType, object? parameter, string language) =>
        throw new NotSupportedException();
}
