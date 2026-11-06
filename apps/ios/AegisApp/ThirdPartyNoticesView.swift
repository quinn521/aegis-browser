import SwiftUI

// Keep the generated text intact; upstream licenses also contain shorter separators.
// This delimiter matches include() in scripts/licenses/third_party_notices.py.
struct ThirdPartyNoticesDocument {
    static let sectionDelimiter = "\n" + String(repeating: "=", count: 72) + "\n"

    struct Notice: Identifiable {
        let id: Int
        let text: String

        var title: String { String(text.prefix(while: { $0 != "\n" })) }
        var body: String { String(text.dropFirst(title.count)) }
    }

    let introduction: String
    let notices: [Notice]

    init(text: String) {
        let sections = text.components(separatedBy: Self.sectionDelimiter)
        introduction = sections[0]
        notices = sections.dropFirst().enumerated().map { Notice(id: $0.offset, text: $0.element) }
    }

    var fullText: String {
        ([introduction] + notices.map(\.text)).joined(separator: Self.sectionDelimiter)
    }
}

struct ThirdPartyNoticesView: View {
    private let document: ThirdPartyNoticesDocument? = Bundle.main.url(forResource: "ThirdPartyNotices", withExtension: "txt")
        .flatMap { try? String(contentsOf: $0, encoding: .utf8) }
        .map { ThirdPartyNoticesDocument(text: $0) }

    var body: some View {
        List {
            Section {
                Text("开源许可与鸣谢")
                    .font(.title.bold())
                    .fixedSize(horizontal: false, vertical: true)
                    .accessibilityAddTraits(.isHeader)
                    .accessibilityIdentifier("third-party-notices-title")
                if let document {
                    NoticeParagraphs(text: document.introduction, identifier: "third-party-notices-introduction")
                }
            }
            if let document {
                Section {
                    ForEach(document.notices) { notice in
                        NavigationLink {
                            ThirdPartyNoticeDetailView(notice: notice)
                        } label: {
                            Text(verbatim: notice.title)
                                .font(.body)
                                .fixedSize(horizontal: false, vertical: true)
                        }
                        .accessibilityIdentifier("third-party-notices-component-\(notice.id)")
                    }
                }
            } else {
                ContentUnavailableView("无法载入许可文件", systemImage: "doc.text")
            }
        }
        .accessibilityIdentifier("third-party-notices-list")
        .navigationTitle("")
        .navigationBarTitleDisplayMode(.inline)
    }
}

private struct ThirdPartyNoticeDetailView: View {
    let notice: ThirdPartyNoticesDocument.Notice

    var body: some View {
        ScrollView {
            LazyVStack(alignment: .leading, spacing: 16) {
                Text(verbatim: notice.title)
                    .font(.title.bold())
                    .fixedSize(horizontal: false, vertical: true)
                    .textSelection(.enabled)
                    .accessibilityAddTraits(.isHeader)
                    .accessibilityIdentifier("third-party-notice-detail-title")
                NoticeParagraphs(text: notice.body, identifier: "third-party-notice-detail-paragraph")
            }
            .frame(maxWidth: 720, alignment: .leading)
            .padding()
            .frame(maxWidth: .infinity)
        }
        .accessibilityIdentifier("third-party-notice-detail-scroll")
        .navigationTitle("")
        .navigationBarTitleDisplayMode(.inline)
    }
}

private struct NoticeParagraphs: View {
    let text: String
    let identifier: String

    var body: some View {
        let paragraphs = text.components(separatedBy: "\n\n")
        ForEach(paragraphs.indices, id: \.self) { index in
            Text(verbatim: paragraphs[index])
                .font(.body)
                .textSelection(.enabled)
                .fixedSize(horizontal: false, vertical: true)
                .frame(maxWidth: .infinity, alignment: .leading)
                .accessibilityIdentifier("\(identifier)-\(index)")
        }
    }
}
