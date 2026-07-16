package agentpolicy

// Violation identifies one policy rule that an input violates.
type Violation struct {
	Rule    string
	Message string
}

// IssueInput contains the fields needed to validate a ChatNow Issue.
type IssueInput struct {
	Title         string
	Body          string
	TargetVersion string
	IsEmergency   bool
}

// PullRequestInput contains the fields needed to validate a ChatNow pull
// request without performing event, filesystem, or Git I/O.
type PullRequestInput struct {
	Title        string
	Body         string
	Head         string
	Base         string
	IssueNumber  int
	ChangedFiles []string
}

// SkillSyncInput contains the pull request declaration and changed paths used
// to enforce repository Skill synchronization.
type SkillSyncInput struct {
	Body         string
	ChangedFiles []string
}
