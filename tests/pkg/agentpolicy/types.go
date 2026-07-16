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
