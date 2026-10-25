/* The three states every data panel can be in besides "showing data". */

export function Loading({ what }: { what: string }) {
  return <p className="status" role="status">Loading {what}…</p>;
}

export function Empty({ message }: { message: string }) {
  return <p className="status">{message}</p>;
}

export function ErrorBox({ message, onRetry }: { message: string; onRetry: () => void }) {
  return (
    <div className="status error" role="alert">
      <p>Could not load the data: {message}</p>
      <button type="button" onClick={onRetry}>Try again</button>
    </div>
  );
}
