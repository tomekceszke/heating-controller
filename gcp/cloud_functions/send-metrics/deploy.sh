/Users/tomek/google-cloud-sdk/bin/gcloud functions deploy send-metrics \
--gen2 \
--runtime=java17 \
--region=europe-west3 \
--source=. \
--entry-point=com.ceszke.heatingcontroller.MetricsSender \
--memory=256MB \
--set-env-vars DATASET=heating_controller_ds \
--set-env-vars TABLE=temperature_raw \
--trigger-http