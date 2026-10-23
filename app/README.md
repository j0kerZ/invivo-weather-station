# Weather station cloud app (AWS Amplify Gen 2)

Scaffolded from the official [Amplify React + Vite template](https://github.com/aws-samples/amplify-vite-react-template);
the template's sample backend and to-do app were removed.

| Path | Contents |
|---|---|
| `amplify/backend.ts` | Backend entry point |
| `amplify/iot/resource.ts` | Station identities on AWS IoT Core (plain CDK) |
| `src/` | Web dashboard (placeholder until step 3) |

```sh
npm install
npx ampx sandbox --once --profile fs   # deploy a personal sandbox; writes amplify_outputs.json
npx ampx sandbox delete --profile fs   # remove it
```

See the repository [README](../README.md) for station credentials and the emulator.
