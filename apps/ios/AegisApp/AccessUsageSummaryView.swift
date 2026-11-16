import SwiftUI

/// Visualization only. Missing and unlimited quotas never become zero-use meters.
struct AccessUsageSummaryView: View {
    let usedBytes: String?
    let quotaBytes: String?

    var body: some View {
        if let fraction = AccessAccountUsagePresentation.usedFraction(used: usedBytes, quota: quotaBytes) {
            ProgressView(value: fraction)
                .tint(.accentColor)
                .accessibilityLabel("总用量")
                .accessibilityValue(Text(fraction, format: .percent.precision(.fractionLength(0))))
                .accessibilityIdentifier("access-usage-progress")
        }
    }
}
