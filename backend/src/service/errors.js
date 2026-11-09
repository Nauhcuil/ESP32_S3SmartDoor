export class ServiceError extends Error {
    code;
    httpStatus;
    constructor(code, message, httpStatus){
        super(message), this.code = code, this.httpStatus = httpStatus;
        this.name = 'ServiceError';
    }
}
