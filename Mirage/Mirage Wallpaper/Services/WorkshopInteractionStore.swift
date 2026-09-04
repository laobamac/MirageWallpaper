//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import Combine
import Foundation
import Observation

@MainActor
@Observable
final class WorkshopInteractionStore {
    private(set) var favoriteIDs: Set<String> = []
    private(set) var changingFavoriteIDs: Set<String> = []
    private(set) var favoriteActionError: String?
    private(set) var favoriteActionErrorItemID: String?

    private(set) var comments: [WorkshopComment] = []
    private(set) var commentsTotal = 0
    private(set) var commentsStartIndex = 0
    private(set) var commentsNextStartIndex = 0
    private(set) var commentsCanPost = false
    private(set) var commentsItemID: String?
    private(set) var isLoadingComments = false
    private(set) var commentsError: String?
    private(set) var commentAuthors: [String: WorkshopCreator] = [:]
    var commentDraft = ""
    private(set) var isPostingComment = false

    @ObservationIgnored private var favoriteCancellable: AnyCancellable?
    @ObservationIgnored private var commentAuthorTask: Task<Void, Never>?
    @ObservationIgnored private var commentGeneration = 0
    @ObservationIgnored private var sessionGeneration = 0
    @ObservationIgnored private var isSteamReady: () -> Bool = { false }
    @ObservationIgnored private var openSteamSetup: () -> Void = {}
    @ObservationIgnored private var onFavoritesChanged: () -> Void = {}

    private let commentsPageSize = 20

    init() {
        favoriteCancellable = SteamServiceManager.shared.$workshopFavoriteIDs
            .receive(on: RunLoop.main)
            .sink { [weak self] favoriteIDs in
                guard let self else { return }
                if self.favoriteIDs != favoriteIDs {
                    self.favoriteIDs = favoriteIDs
                }
                self.onFavoritesChanged()
            }
    }

    func configure(
        isSteamReady: @escaping () -> Bool,
        openSteamSetup: @escaping () -> Void,
        onFavoritesChanged: @escaping () -> Void
    ) {
        self.isSteamReady = isSteamReady
        self.openSteamSetup = openSteamSetup
        self.onFavoritesChanged = onFavoritesChanged
    }

    func isFavorite(_ workshopID: String) -> Bool {
        favoriteIDs.contains(workshopID)
    }

    func favoriteError(for workshopID: String) -> String? {
        favoriteActionErrorItemID == workshopID ? favoriteActionError : nil
    }

    func dismissFavoriteError() {
        favoriteActionError = nil
        favoriteActionErrorItemID = nil
    }

    func toggleFavorite(_ item: WorkshopItem) {
        toggleFavorite(workshopID: item.publishedFileId)
    }

    func toggleFavorite(workshopID: String) {
        guard isSteamReady() else {
            openSteamSetup()
            return
        }
        guard !workshopID.isEmpty, !changingFavoriteIDs.contains(workshopID) else { return }
        let favorited = !isFavorite(workshopID)
        let requestSession = sessionGeneration
        changingFavoriteIDs.insert(workshopID)
        favoriteActionError = nil
        favoriteActionErrorItemID = workshopID
        SteamServiceManager.shared.setWorkshopFavorite(
            workshopId: workshopID,
            favorited: favorited
        ) { [weak self] result in
            guard let self, requestSession == self.sessionGeneration else { return }
            self.changingFavoriteIDs.remove(workshopID)
            switch result {
            case .success:
                self.favoriteActionError = nil
                self.favoriteActionErrorItemID = nil
            case .failure(let error):
                self.favoriteActionError = error.localizedDescription
                self.favoriteActionErrorItemID = workshopID
            }
        }
    }

    func prepareComments(for item: WorkshopItem) {
        if commentsItemID != item.publishedFileId {
            loadComments(for: item, startIndex: 0)
        }
    }

    func loadComments(for item: WorkshopItem, startIndex: Int = 0) {
        commentAuthorTask?.cancel()
        commentGeneration += 1
        guard SteamServiceManager.shared.isLoggedIn else {
            resetComments(for: item.publishedFileId, error: L("需要登录 Steam"))
            return
        }
        guard !item.creatorSteamId.isEmpty else {
            resetComments(
                for: item.publishedFileId,
                error: L("该作品缺少可用的作者信息，无法加载评论")
            )
            return
        }
        let requestGeneration = commentGeneration
        let requestSession = sessionGeneration
        let requestedStart = max(0, startIndex)
        if commentsItemID != item.publishedFileId {
            comments = []
            commentsTotal = 0
            commentsStartIndex = 0
            commentsNextStartIndex = 0
            commentDraft = ""
            commentsCanPost = false
            commentAuthors = [:]
        }
        commentsItemID = item.publishedFileId
        commentsError = nil
        isLoadingComments = true
        SteamServiceManager.shared.fetchComments(
            item: item,
            startIndex: requestedStart,
            count: commentsPageSize
        ) { [weak self] result in
            guard let self,
                  requestSession == self.sessionGeneration,
                  requestGeneration == self.commentGeneration,
                  self.commentsItemID == item.publishedFileId else { return }
            switch result {
            case .success(let page):
                self.comments = page.comments
                self.commentsTotal = page.total
                self.commentsStartIndex = page.startIndex
                self.commentsNextStartIndex = page.nextStartIndex
                self.commentsCanPost = page.canPost
                self.loadCommentAuthors(
                    for: page.comments,
                    itemID: item.publishedFileId,
                    generation: requestGeneration,
                    session: requestSession
                )
            case .failure(let error):
                self.commentsError = error.localizedDescription
            }
            self.isLoadingComments = false
        }
    }

    func refreshComments(for item: WorkshopItem) {
        loadComments(for: item, startIndex: commentsStartIndex)
    }

    func loadPreviousComments(for item: WorkshopItem) {
        guard commentsStartIndex > 0, !isLoadingComments else { return }
        loadComments(for: item, startIndex: max(0, commentsStartIndex - commentsPageSize))
    }

    func loadNextComments(for item: WorkshopItem) {
        guard commentsNextStartIndex > commentsStartIndex,
              commentsNextStartIndex < commentsTotal,
              !isLoadingComments else { return }
        loadComments(for: item, startIndex: commentsNextStartIndex)
    }

    func postComment(for item: WorkshopItem) {
        let text = commentDraft.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !text.isEmpty, commentsCanPost, !isPostingComment else { return }
        let requestSession = sessionGeneration
        isPostingComment = true
        commentsError = nil
        SteamServiceManager.shared.postComment(item: item, text: text) { [weak self] result in
            guard let self, requestSession == self.sessionGeneration else { return }
            self.isPostingComment = false
            guard self.commentsItemID == item.publishedFileId else { return }
            switch result {
            case .success:
                self.commentDraft = ""
                self.loadComments(for: item, startIndex: 0)
            case .failure(let error):
                self.commentsError = error.localizedDescription
            }
        }
    }

    func reset() {
        sessionGeneration += 1
        commentGeneration += 1
        commentAuthorTask?.cancel()
        commentAuthorTask = nil
        favoriteIDs = []
        changingFavoriteIDs = []
        favoriteActionError = nil
        favoriteActionErrorItemID = nil
        resetComments(for: nil, error: nil)
        commentDraft = ""
        isPostingComment = false
    }

    private func loadCommentAuthors(
        for comments: [WorkshopComment],
        itemID: String,
        generation: Int,
        session: Int
    ) {
        let IDs = Array(Set(comments.map(\.authorSteamId).filter { !$0.isEmpty })).sorted()
        guard !IDs.isEmpty else {
            commentAuthors = [:]
            return
        }
        commentAuthorTask?.cancel()
        commentAuthorTask = Task { @MainActor [weak self] in
            var profiles: [String: WorkshopCreator] = [:]
            for ID in IDs {
                guard !Task.isCancelled else { return }
                if let creator = await SteamWebAPI.shared.creatorProfile(steamId: ID) {
                    profiles[ID] = creator
                }
            }
            guard !Task.isCancelled,
                  let self,
                  self.sessionGeneration == session,
                  self.commentGeneration == generation,
                  self.commentsItemID == itemID else { return }
            self.commentAuthors.merge(profiles) { _, new in new }
        }
    }

    private func resetComments(for itemID: String?, error: String?) {
        comments = []
        commentsTotal = 0
        commentsStartIndex = 0
        commentsNextStartIndex = 0
        commentsCanPost = false
        commentsItemID = itemID
        commentsError = error
        commentAuthors = [:]
        isLoadingComments = false
    }
}
